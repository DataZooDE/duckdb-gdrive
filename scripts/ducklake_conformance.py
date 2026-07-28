#!/usr/bin/env python3
"""Run DuckLake's own test suite with its DATA PATH on gdrive://.

    make ducklake_conformance

Why: DuckLake exercises far more of a filesystem than our own tests do --
appends, updates, deletes, compaction, schema evolution, time travel,
transaction rollback, partitioning. Each of those is a different pattern of
writes, globs, overwrites and reads. Borrowing the suite is much cheaper than
inventing equivalent coverage, and it is written by people who do not care
about our implementation, which is what makes it worth running.

THE METHOD IS DIFFERENTIAL, and that is the whole point.

Every selected test runs TWICE: once with the data path on a local temp
directory, once with it on gdrive://. Only tests that PASS LOCALLY and FAIL ON
DRIVE are reported as findings. Anything failing both ways is failing for
reasons that have nothing to do with us -- a missing fixture, a DuckLake
version difference, an unsupported option -- and reporting those would bury
the real signal in noise.

So the output is not "N tests passed". It is "these specific operations work
on a local filesystem and do not work on ours", which is a list of bugs.

Caveats, stated because they bound what this proves:
  * Tests needing {DUCKLAKE_CONNECTION} (a Postgres/MySQL catalog in their CI)
    are skipped -- we cannot supply one.
  * The CATALOG stays local in both variants. Only the DATA path moves. That
    is the supported configuration; a DuckDB database file on gdrive:// is
    explicitly not (Drive has no atomic rename).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
UNITTEST = REPO / "build" / "release" / "test" / "unittest"
CACHE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "ducklake-conformance"
OUT = REPO / "test" / "sql" / "live" / "ducklake_conf"

DUCKLAKE_REPO = "https://github.com/duckdb/ducklake.git"

#: ATTACH with a local catalog. The ones we can redirect.
ATTACH_RE = re.compile(
    r"""ATTACH\s+'ducklake:(?!\{DUCKLAKE_CONNECTION\})(?P<cat>[^']+)'\s+AS\s+(?P<alias>\w+)(?P<opts>\s*\([^)]*\))?""",
    re.IGNORECASE,
)


def ensure_ducklake(ref: str) -> Path:
    """Clone DuckLake's tests once, into a cache outside the repo."""
    d = CACHE / "ducklake"
    if not (d / "test").is_dir():
        d.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--depth", "1", "--branch", ref, DUCKLAKE_REPO, str(d)],
                       check=True, capture_output=True)
    return d


def rewrite(text: str, data_root: str) -> tuple[str, int]:
    """Point every ATTACH's DATA_PATH at `data_root`. Returns (text, count)."""
    n = 0

    def sub(m: re.Match) -> str:
        nonlocal n
        n += 1
        alias = m.group("alias")
        opts = (m.group("opts") or "").strip()
        target = f"{data_root}/{alias}_{n}/"
        if not opts:
            return f"{m.group(0)} (DATA_PATH '{target}')"
        # Replace an existing DATA_PATH, or add one to the option list.
        inner = opts[1:-1].strip()
        if re.search(r"DATA_PATH", inner, re.IGNORECASE):
            inner = re.sub(r"DATA_PATH\s+'[^']*'", f"DATA_PATH '{target}'", inner, flags=re.IGNORECASE)
        else:
            inner = f"{inner}, DATA_PATH '{target}'" if inner else f"DATA_PATH '{target}'"
        head = m.group(0)[: m.group(0).rindex("(")].rstrip()
        return f"{head} ({inner})"

    return ATTACH_RE.sub(sub, text), n


def prepare(src: Path, dest: Path, data_root: str, secret_dir: str | None) -> bool:
    text = src.read_text()
    if "{DUCKLAKE_CONNECTION}" in text:
        return False
    body, n = rewrite(text, data_root)
    if n == 0:
        return False

    # `require ducklake` skips the whole file in our unittest binary, which
    # runs with autoinstall off. We LOAD it explicitly in the prelude, so the
    # directive is removed rather than allowed to skip -- a skipped file is
    # not a comparison.
    body = re.sub(r"^require\s+ducklake\s*$", "", body, flags=re.MULTILINE)

    header, _, rest = body.partition("\n\n")
    # `require gdrive` REGISTERS the filesystem. Without it a gdrive:// path
    # falls through to DuckDB's LocalFileSystem, which fails with
    # "Failed to create directory ... No such file or directory" -- an error
    # that looks like our bug and is not. That cost a wrong diagnosis once.
    prelude = "\nrequire gdrive\n\nstatement ok\nLOAD ducklake;\n" if secret_dir else "\nstatement ok\nLOAD ducklake;\n"
    if secret_dir:
        # A PERSISTENT secret, referenced by directory. Deliberately not an
        # inline CREATE SECRET: that writes the real client secret and refresh
        # token into a file on disk, and the first version of this harness did
        # exactly that -- into a path .gitignore did not actually cover.
        # Nothing here should ever contain a credential.
        prelude += f"\nstatement ok\nSET secret_directory='{secret_dir}';\n"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(f"{header}\n{prelude}\n{rest}")
    return True


def make_secret_dir() -> str:
    """A 0700 directory holding one persistent gdrive secret.

    Credentials are read from the environment and written ONLY here, by
    DuckDB, as a 0600 file outside the repository. Generated tests reference
    the directory; they never contain a credential.
    """
    d = tempfile.mkdtemp(prefix="gdrive-dlconf-secrets-")
    os.chmod(d, 0o700)
    sql = (
        f"SET secret_directory='{d}';\n"
        "CREATE PERSISTENT SECRET dlconf (TYPE gdrive, PROVIDER config, "
        f"CLIENT_ID '{os.environ['GDRIVE_OAUTH_CLIENT_ID']}', "
        f"CLIENT_SECRET '{os.environ['GDRIVE_OAUTH_CLIENT_SECRET']}', "
        f"REFRESH_TOKEN '{os.environ['GDRIVE_USER_REFRESH_TOKEN']}', "
        f"ROOT_FOLDER_ID '{os.environ['GDRIVE_CI_DRIVE_ID']}', "
        "DRIVE_SCOPE 'https://www.googleapis.com/auth/drive');"
    )
    cli = REPO / "build" / "release" / "duckdb"
    r = subprocess.run([str(cli), "-c", sql], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise SystemExit("could not create the persistent secret")
    return d


def run_one(path: Path, timeout: int) -> tuple[bool, str]:
    """Run one generated test. Exit 0 is NOT sufficient evidence of a pass.

    unittest exits 0 in at least three situations that are not success:
      * "No test cases matched" -- it rejects an ABSOLUTE path, so passing one
        silently runs nothing. The first version of this harness did exactly
        that and reported 43 agreements having executed zero tests.
      * "All tests were skipped" -- an unmet `require`.
      * an empty filter matching nothing.

    Each of those means THE COMPARISON DID NOT HAPPEN, which must never be
    counted as the two sides agreeing.
    """
    rel = path.relative_to(REPO) if path.is_absolute() else path
    p = subprocess.run([str(UNITTEST), str(rel)], capture_output=True, text=True,
                       timeout=timeout, cwd=REPO)
    out = p.stdout + p.stderr
    if "No test cases matched" in out or "No tests ran" in out:
        return False, "NOT-RUN"
    if "All tests were skipped" in out:
        return False, "SKIPPED"
    if "assertions" not in out and "All tests passed" not in out:
        # Nothing that looks like a result. Refuse to guess.
        return False, "NO-RESULT"
    return p.returncode == 0, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="main", help="DuckLake git ref to borrow tests from")
    ap.add_argument("--limit", type=int, default=25, help="how many test files to run (0 = all)")
    ap.add_argument("--filter", default="", help="only files whose path contains this")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    if not UNITTEST.is_file():
        print(f"FAIL: {UNITTEST} not built. Run `make` first.", file=sys.stderr)
        return 2
    for var in ("GDRIVE_CI_DRIVE_ID", "GDRIVE_OAUTH_CLIENT_ID", "GDRIVE_OAUTH_CLIENT_SECRET",
                "GDRIVE_USER_REFRESH_TOKEN"):
        if not os.environ.get(var):
            print(f"FAIL: {var} not set -- this suite writes, so it needs the delegated user.",
                  file=sys.stderr)
            return 2

    cli = REPO / "build" / "release" / "duckdb"
    if subprocess.run([str(cli), "-c", "INSTALL ducklake; LOAD ducklake;"],
                      capture_output=True, text=True, timeout=180).returncode != 0:
        print("FAIL: could not install ducklake", file=sys.stderr)
        return 2

    dl = ensure_ducklake(args.ref)
    candidates = sorted(p for p in (dl / "test" / "sql").rglob("*.test")
                        if args.filter in str(p))
    if args.limit:
        candidates = candidates[: args.limit]
    print(f"==> {len(candidates)} candidate DuckLake test file(s) from {args.ref}")

    run_id = f"run-dlconf-{uuid.uuid4().hex[:8]}"
    secret_dir = make_secret_dir()

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    findings, agreed, both_failed, skipped = [], 0, 0, 0
    for src in candidates:
        stem = src.relative_to(dl / "test" / "sql").as_posix().replace("/", "_")
        local_f = OUT / f"local_{stem}"
        drive_f = OUT / f"drive_{stem}"
        if not prepare(src, local_f, "__TEST_DIR__/dlconf", None):
            continue
        if not prepare(src, drive_f, f"gdrive://scratch/{run_id}", secret_dir):
            continue

        try:
            local_ok, _ = run_one(local_f, args.timeout)
        except subprocess.TimeoutExpired:
            local_ok = False
        if not local_ok:
            both_failed += 1
            continue  # fails without us; nothing to learn

        try:
            drive_ok, drive_out = run_one(drive_f, args.timeout)
        except subprocess.TimeoutExpired:
            drive_ok, drive_out = False, "TIMEOUT"
        if drive_ok:
            agreed += 1
        elif drive_out in ("SKIPPED", "NOT-RUN", "NO-RESULT"):
            skipped += 1
            print(f"  NOT COMPARED ({drive_out})  {src.relative_to(dl)}")
        else:
            findings.append((src.relative_to(dl).as_posix(), drive_out))
            print(f"  FINDING  {src.relative_to(dl)}")

    print()
    print(f"  passed on local AND gdrive : {agreed}")
    print(f"  failed on local too (ignored): {both_failed}")
    print(f"  skipped on gdrive           : {skipped}")
    print(f"  PASS LOCAL, FAIL GDRIVE     : {len(findings)}")

    for name, out in findings:
        print(f"\n----- {name} -----")
        lines = [l for l in out.splitlines() if l.strip()]
        for l in lines[-25:]:
            print(f"  {l}")

    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
