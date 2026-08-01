"""Capture the three span traces behind the paper's timeline figure.

    make trace_figure

Runs three workloads, each in its OWN duckdb process with GDRIVE_TRACE_FILE
set, and writes the traces to docs/paper/data/. A fresh process per workload
is not tidiness: the panels are about cold behaviour, and a reused process
would carry a warm path cache and a live TLS connection into the next panel,
erasing exactly the serial prologue the figure exists to show.

Panels:
  (a) cold Parquet scan of the 87 MB fixture -- the benchmark query
  (b) cold DuckLake read  -- fresh process against a catalog written in (c)
  (c) DuckLake write      -- CREATE TABLE AS onto gdrive://

(c) runs before (b) despite the ordering, because (b) needs something to read.
The catalog is a local temp file in both; only DATA_PATH is on Drive, which is
the supported shape.

Writes go to gdrive://scratch/<run-id>/ and are left behind for inspection --
`make sweep_orphans` collects them. Needs the delegated user: a service
account has no Drive storage quota.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CLI = REPO / "build" / "release" / "duckdb"
OUT = REPO / "docs" / "paper" / "data"

REQUIRED = ("GDRIVE_CI_DRIVE_ID", "GDRIVE_OAUTH_CLIENT_ID",
            "GDRIVE_OAUTH_CLIENT_SECRET", "GDRIVE_USER_REFRESH_TOKEN")

SCAN_QUERY = ("SELECT count(*), sum(id), min(s1), max(s1) "
              "FROM read_parquet('gdrive://fixtures/wide.parquet');")


def secret_sql() -> str:
    # PROVIDER config with the delegated user's refresh token. Built here and
    # passed on the command line of a child process -- acceptable only because
    # this is a developer's own machine running their own credential; CI does
    # not run this target.
    return (
        "CREATE SECRET t (TYPE gdrive, PROVIDER config,"
        f" CLIENT_ID '{os.environ['GDRIVE_OAUTH_CLIENT_ID']}',"
        f" CLIENT_SECRET '{os.environ['GDRIVE_OAUTH_CLIENT_SECRET']}',"
        f" REFRESH_TOKEN '{os.environ['GDRIVE_USER_REFRESH_TOKEN']}',"
        f" ROOT_FOLDER_ID '{os.environ['GDRIVE_CI_DRIVE_ID']}',"
        " DRIVE_SCOPE 'https://www.googleapis.com/auth/drive');"
    )


def run(name: str, sql: str, timeout: int = 900) -> int:
    dest = OUT / f"trace_{name}.jsonl"
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        dest.unlink()
    env = {**os.environ, "GDRIVE_TRACE_FILE": str(dest)}
    print(f"==> {name}", file=sys.stderr, flush=True)
    p = subprocess.run([str(CLI), "-c", sql], env=env, capture_output=True,
                       text=True, timeout=timeout)
    if p.returncode != 0:
        print(p.stdout[-2000:] + p.stderr[-2000:], file=sys.stderr)
        raise SystemExit(f"FAIL: {name} exited {p.returncode}")
    # A trace with only the header means the workload never reached Drive --
    # a green exit and an empty figure is the worst combination here.
    spans = sum(1 for line in dest.read_text().splitlines()
                if '"record":"header"' not in line)
    if spans == 0:
        raise SystemExit(f"FAIL: {name} produced no spans; the figure would be empty")
    print(f"    {spans} spans -> {dest.relative_to(REPO)}", file=sys.stderr)
    return spans


def main() -> int:
    if not CLI.is_file():
        print(f"FAIL: {CLI} not built. Run `make` first.", file=sys.stderr)
        return 2
    missing = [v for v in REQUIRED if not os.environ.get(v)]
    if missing:
        print(f"FAIL: {', '.join(missing)} not set -- panel (c) writes, so this "
              "needs the delegated user.", file=sys.stderr)
        return 2

    run_id = f"run-paper-{uuid.uuid4().hex[:8]}"
    catalog = Path(tempfile.mkdtemp(prefix="gdrive-trace-")) / "lake.db"
    secret = secret_sql()
    attach = (f"ATTACH 'ducklake:{catalog}' AS lake "
              f"(DATA_PATH 'gdrive://scratch/{run_id}/lake/'); USE lake;")

    print(f"scratch: gdrive://scratch/{run_id}/", file=sys.stderr)

    run("scan", f"{secret} {SCAN_QUERY}")
    run("ducklake_write",
        f"INSTALL ducklake; LOAD ducklake; {secret} {attach} "
        "CREATE TABLE t AS SELECT i AS id, i%7 AS grp, md5(i::VARCHAR) AS s "
        "FROM range(200000) tt(i);")
    run("ducklake_read",
        f"LOAD ducklake; {secret} {attach} "
        "SELECT count(*), sum(id), count(DISTINCT grp) FROM t;")

    print("\nNow regenerate the figure:  cd docs/paper && make figures",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
