#!/usr/bin/env python3
"""Check that the README documents things that actually exist.

    make verify_readme

BRD criterion 1 is "correct against real Drive, clean install, README only".
The README is therefore part of the product, and documentation drift is a
real defect: a reader who copies a documented `CREATE SECRET` and gets
"Binder Error: Unknown parameter" has been handed a broken product, however
green the test suite is.

This is not hypothetical here. `SCOPE` was renamed to `DRIVE_SCOPE` because
`SCOPE` is a reserved DuckDB clause that the parser consumes before an
extension sees it -- a secret written the documented way matched nothing and
every query failed with "no secret configured". Had the README kept the old
spelling, nothing in the suite would have noticed.

What is checked, and what is deliberately not:

  * CREATE SECRET blocks are EXECUTED with their documented parameters.
    DuckDB rejects unknown parameters and unknown providers at bind time, so
    this catches a renamed or removed option exactly. The credential values
    are placeholders; a secret is not validated against Google until used.
  * SET statements are EXECUTED, which checks the setting exists.
  * gdrive_* functions mentioned anywhere in the README must exist in
    duckdb_functions().
  * Plain SELECTs with no placeholder are parse-checked.

  * Statements that read or write gdrive:// paths are NOT executed. They
    need credentials and real file ids; that is what the live suite is for.
    Pretending otherwise here would just duplicate it badly.
  * Statements containing `...` are ILLUSTRATIVE by construction ("INSERT
    INTO t SELECT ...") and cannot be parsed. They are skipped and COUNTED,
    so the number of unchecked statements is visible rather than implied.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
README = REPO_ROOT / "README.md"
DUCKDB = REPO_ROOT / "build" / "release" / "duckdb"

SQL_BLOCK = re.compile(r"```sql\n(.*?)```", re.DOTALL)


def run_sql(sql: str) -> tuple[int, str]:
    proc = subprocess.run(
        [str(DUCKDB), "-noheader", "-list", "-c", sql],
        capture_output=True, text=True, timeout=120,
    )
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def split_statements(block: str) -> list[str]:
    """Strip -- comments FIRST, then split on ';'.

    Order matters: splitting first turns `-- Catalog is local; data is not.`
    into a comment and a bare fragment "data is not." that looks like a
    statement and fails to parse. That is a bug in the checker, reported as
    a bug in the README -- the worst kind of false alarm, because it trains
    people to ignore the check.
    """
    no_comments = "\n".join(
        line.split("--", 1)[0] for line in block.splitlines()
    )
    return [s.strip() for s in no_comments.split(";") if s.strip()]


#: Errors that mean the statement BOUND correctly and then failed on
#: something this checker cannot supply. Reaching one of these is a pass: it
#: proves every parameter name, provider and setting was accepted.
#:
#: The README documents `KEY_FILE '/etc/creds/sa.json'`, and the extension
#: validates the key file eagerly at CREATE SECRET time (good behaviour --
#: fail fast rather than at first read). So the documented example CANNOT
#: succeed here, and demanding success would mean either weakening the
#: extension or writing a README that does not say what users should type.
BOUND_THEN_FAILED = (
    "key file not found",
    "no such file",
)

#: Errors that mean the DOCUMENTATION IS WRONG. These are the point.
DRIFT_MARKERS = (
    "unknown parameter",
    "binder error",
    "parser error",
    "syntax error",
    "does not exist",
    "unsupported provider",
    "unrecognized configuration parameter",
)


def binding_succeeded(out: str) -> bool:
    low = out.lower()
    if any(m in low for m in DRIFT_MARKERS):
        return False
    return any(m in low for m in BOUND_THEN_FAILED)


def main() -> int:
    if not DUCKDB.is_file():
        print(f"FAIL: {DUCKDB} not built. Run `make` first.", file=sys.stderr)
        return 2

    text = README.read_text()
    blocks = SQL_BLOCK.findall(text)
    if not blocks:
        print("FAIL: no ```sql blocks found in README.md -- has the format changed?",
              file=sys.stderr)
        return 1

    statements = [s for b in blocks for s in split_statements(b)]
    print(f"==> {len(blocks)} SQL block(s), {len(statements)} statement(s)")

    failures = []

    # --- gdrive_* functions the README mentions must exist -----------------
    mentioned = sorted(set(re.findall(r"\bgdrive_[a-z_]+\s*\(", text)))
    mentioned = [m.rstrip("( ").strip() for m in mentioned]
    if mentioned:
        rc, out = run_sql(
            "SELECT function_name FROM duckdb_functions() WHERE function_name LIKE 'gdrive%'"
        )
        have = set(out.splitlines())
        for fn in mentioned:
            if fn not in have:
                failures.append(f"README mentions {fn}() but it is not a registered function")
        print(f"    functions: {', '.join(mentioned)} -> "
              f"{len([m for m in mentioned if m in have])}/{len(mentioned)} exist")

    # --- executable statements ---------------------------------------------
    checked = 0
    for stmt in statements:
        head = stmt.lstrip().upper()
        executable = head.startswith("CREATE SECRET") or head.startswith("SET ")
        if not executable:
            continue
        # A secret with the same name may already exist from a previous block.
        sql = stmt + ";"
        if head.startswith("CREATE SECRET"):
            sql = "SET autoinstall_known_extensions=false;\n" + \
                  re.sub(r"^(\s*CREATE\s+SECRET)", r"CREATE OR REPLACE SECRET", stmt,
                         flags=re.IGNORECASE) + ";"
        rc, out = run_sql(sql)
        checked += 1
        if rc != 0 and not binding_succeeded(out):
            first = stmt.strip().splitlines()[0][:70]
            failures.append(f"documented statement failed: {first}...\n      {out.splitlines()[0] if out else ''}")

    print(f"    executed: {checked} CREATE SECRET / SET statement(s)")

    # --- the SCOPE trap ------------------------------------------------------
    #
    # This one CANNOT be caught by executing the statement, which is exactly
    # why it cost a day. `SCOPE` is a reserved DuckDB clause meaning "which
    # paths may use this secret". Writing SCOPE '<oauth-scope-url>' does not
    # error -- DuckDB happily sets the secret's path scope to that URL, the
    # secret then matches no gdrive:// path, and every query fails with "no
    # secret configured" while the CREATE SECRET looked perfect.
    #
    # Verified: a correct gdrive secret has scope []; the trap produces
    # ['https://www.googleapis.com/auth/drive'].
    for stmt in statements:
        if not stmt.lstrip().upper().startswith("CREATE SECRET"):
            continue
        if "TYPE GDRIVE" not in stmt.upper().replace("  ", " "):
            continue
        probe = re.sub(r"KEY_FILE\s+'[^']*'",
                       f"KEY_FILE '{REPO_ROOT / 'test/cpp/testdata/fake_sa_key.json'}'",
                       stmt, flags=re.IGNORECASE)
        # Rename to a known probe name and query ONLY that row. Querying all
        # of duckdb_secrets() picked up DuckDB's built-in
        # __default_http_basic, whose scope legitimately IS an http URL --
        # a false positive that would have made this check untrustworthy.
        probe = re.sub(r"^\s*CREATE\s+SECRET\s+\S+",
                       "CREATE OR REPLACE SECRET readme_scope_probe", probe,
                       flags=re.IGNORECASE)
        rc, out = run_sql(
            probe + ";\nSELECT unnest(scope) FROM duckdb_secrets() "
            "WHERE name = 'readme_scope_probe';")
        bad = [line for line in out.splitlines() if line.startswith("http")]
        if bad:
            failures.append(
                "a documented gdrive CREATE SECRET has an HTTP URL in its PATH scope: "
                f"{bad[0]}\n      This is the SCOPE/DRIVE_SCOPE trap -- SCOPE is a reserved "
                "DuckDB clause meaning\n      'which paths may use this secret'. It does not "
                "error; the secret simply\n      matches nothing. The OAuth scope belongs in "
                "DRIVE_SCOPE.")

    # --- settings named ANYWHERE in the README, prose included ---------------
    #
    # `SET gdrive_docs_export_mime=...` appears as inline code in a
    # paragraph, not in a ```sql block. Scanning only the code blocks missed
    # both documented settings entirely -- and a mutation test renaming one
    # of them passed cleanly, which is how this was found.
    named_settings = sorted(set(re.findall(r"SET\s+(gdrive_[a-z_]+)\s*=", text)))
    if named_settings:
        rc, out = run_sql("SELECT name FROM duckdb_settings() WHERE name LIKE 'gdrive%'")
        have = set(out.splitlines())
        for s in named_settings:
            if s not in have:
                failures.append(f"README documents SET {s} but no such setting is registered")
        print(f"    settings: {', '.join(named_settings)} -> "
              f"{len([s for s in named_settings if s in have])}/{len(named_settings)} exist")

    # --- plain SELECTs must parse -------------------------------------------
    #
    # json_serialize_sql only accepts SELECT, and README SQL is legitimately
    # illustrative in places ("INSERT INTO t SELECT ..."), so this checks the
    # subset it can honestly check and SAYS how much it skipped.
    parsed = 0
    skipped = []
    for stmt in statements:
        head = stmt.lstrip().upper()
        if head.startswith("CREATE SECRET") or head.startswith("SET "):
            continue
        if "..." in stmt or not head.startswith("SELECT"):
            skipped.append(stmt.splitlines()[0][:60])
            continue
        escaped = stmt.replace("'", "''")
        rc, out = run_sql(
            f"INSTALL json; LOAD json; SELECT json_serialize_sql('{escaped}') AS s"
        )
        parsed += 1
        if rc != 0 or '"error":true' in out:
            first = stmt.strip().splitlines()[0][:70]
            failures.append(f"documented SQL does not parse: {first}...\n      {out[:200]}")

    print(f"    parsed:   {parsed} plain SELECT(s)")
    if skipped:
        print(f"    skipped:  {len(skipped)} statement(s) not parse-checkable "
              f"(illustrative, or not a SELECT):")
        for s in skipped:
            print(f"                {s}")

    if failures:
        print("\nREADME verification FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        print("\nThe README is part of the product (BRD criterion 1 is 'README only').",
              file=sys.stderr)
        return 1

    print("OK: README SQL matches the extension")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
