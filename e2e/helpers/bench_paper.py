"""Fill the paper's remaining measured cells.

    make bench_paper

Three experiments the single-file benchmark does not cover:

  threads   scan wall-clock and Drive API calls against `SET threads`. The
            property under test is not that time falls -- decompression is the
            parallel part, so it will -- but that the request count stays
            FLAT. A rising count means the single-flight caches are leaking,
            which is the regression that produced 19 metadata calls for one
            query before those caches existed.

  write     upload wall-clock by size, folder creation, and a DuckLake commit.

  download  download-then-query: the workflow this extension replaces. Timed
            end to end (fetch the bytes, then scan them locally) because that
            is what the user actually waits for.

Each timing is a FRESH duckdb process, as in helpers.bench: cold means cold,
and a warm path cache or a live TLS connection would flatter every number.
Reported by minimum over `--repeats`.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DUCKDB = REPO / "build" / "release" / "duckdb"

REQUIRED = ("GDRIVE_CI_DRIVE_ID", "GDRIVE_OAUTH_CLIENT_ID",
            "GDRIVE_OAUTH_CLIENT_SECRET", "GDRIVE_USER_REFRESH_TOKEN")

FIXTURE = "gdrive://fixtures/wide.parquet"
SCAN = ("SELECT count(*), sum(id), min(s1), max(s1) FROM read_parquet('{uri}')")


def secret_sql() -> str:
    return (
        "CREATE SECRET b (TYPE gdrive, PROVIDER config,"
        f" CLIENT_ID '{os.environ['GDRIVE_OAUTH_CLIENT_ID']}',"
        f" CLIENT_SECRET '{os.environ['GDRIVE_OAUTH_CLIENT_SECRET']}',"
        f" REFRESH_TOKEN '{os.environ['GDRIVE_USER_REFRESH_TOKEN']}',"
        f" ROOT_FOLDER_ID '{os.environ['GDRIVE_CI_DRIVE_ID']}',"
        " DRIVE_SCOPE 'https://www.googleapis.com/auth/drive');"
    )


def run(statements: list[str]) -> tuple[float, str]:
    """One fresh process. Returns (seconds, last non-empty stdout line).

    Script on STDIN, never argv: it carries a refresh token, and argv is
    world-readable through /proc.
    """
    script = "\n".join(s.rstrip(";") + ";" for s in statements)
    t0 = time.perf_counter()
    p = subprocess.run([str(DUCKDB), "-noheader", "-list"], input=script,
                       capture_output=True, text=True, timeout=1800)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        raise RuntimeError(f"duckdb exited {p.returncode}\n{p.stdout[-1500:]}{p.stderr[-1500:]}")
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    return dt, (lines[-1] if lines else "")


def best(statements, repeats):
    times, out = [], ""
    for _ in range(repeats):
        dt, out = run(statements)
        times.append(dt)
    return min(times), statistics.median(times), out


def threads_sweep(repeats):
    rows = []
    for n in (1, 4, 8, 16, 32):
        mn, med, out = best([
            secret_sql(), f"SET threads={n}", SCAN.format(uri=FIXTURE),
            "SELECT 'calls=' || sum(value) FROM gdrive_stats() "
            "WHERE metric IN ('files_list','files_get','files_media')",
        ], repeats)
        calls = int(out.split("=")[1]) if "calls=" in out else -1
        rows.append({"threads": n, "min_s": round(mn, 2), "median_s": round(med, 2),
                     "calls": calls})
        print(f"  threads={n:<3} min={mn:5.2f}s calls={calls}", file=sys.stderr)
    return rows


def write_path(repeats, run_id):
    rows = []
    for label, mib in (("Upload 1 MiB", 1), ("Upload 16 MiB", 16), ("Upload 128 MiB", 128)):
        # range() rows sized to land near the target; parquet compresses, so
        # the wall-clock is what matters, not an exact byte count.
        n = mib * 1024 * 1024 // 16
        uri = f"gdrive://scratch/{run_id}/w{mib}.parquet"
        mn, med, _ = best([
            secret_sql(),
            f"COPY (SELECT i, md5(i::VARCHAR) AS s FROM range({n}) t(i)) "
            f"TO '{uri}' (FORMAT parquet)",
        ], repeats)
        rows.append({"op": label, "min_s": round(mn, 2), "median_s": round(med, 2)})
        print(f"  {label:<16} min={mn:5.2f}s", file=sys.stderr)

    mn, med, _ = best([secret_sql(),
                       f"COPY (SELECT 1 AS a) TO 'gdrive://scratch/{run_id}/f{uuid.uuid4().hex[:6]}/x.csv' (FORMAT csv)"],
                      repeats)
    rows.append({"op": "Create one folder + tiny file", "min_s": round(mn, 2),
                 "median_s": round(med, 2)})
    print(f"  folder+tiny      min={mn:5.2f}s", file=sys.stderr)
    return rows


def ducklake_ops(repeats, run_id):
    """CTAS / append / delete / cold scan / time travel, DATA_PATH on Drive."""
    rows = []
    for i in range(repeats):
        cat = Path(tempfile.mkdtemp(prefix="dlpaper-")) / "lake.db"
        base = [secret_sql(), "INSTALL ducklake", "LOAD ducklake",
                f"ATTACH 'ducklake:{cat}' AS lake "
                f"(DATA_PATH 'gdrive://scratch/{run_id}/dl{i}/')", "USE lake"]
        steps = [
            ("CREATE TABLE AS", "CREATE TABLE t AS SELECT i AS id, i%7 AS grp, "
                                "md5(i::VARCHAR) AS s FROM range(200000) tt(i)"),
            ("INSERT (append)", "INSERT INTO t SELECT i, i%7, md5(i::VARCHAR) "
                                "FROM range(200000,250000) tt(i)"),
            ("DELETE", "DELETE FROM t WHERE grp = 3"),
            ("Scan, cold", "SELECT count(*), sum(id) FROM t"),
            ("Time-travel read", "SELECT count(*) FROM t AT (VERSION => 1)"),
        ]
        done = []
        for name, sql in steps:
            # Each step in its OWN process, replaying the previous steps'
            # effects via the shared catalog -- so "cold scan" really is cold.
            dt, _ = run(base + done + [sql])
            rows.append({"op": name, "_t": dt, "_iter": i})
            done.append(sql)
            print(f"  [{i}] {name:<18} {dt:5.2f}s", file=sys.stderr)
    agg = {}
    for r in rows:
        agg.setdefault(r["op"], []).append(r["_t"])
    return [{"op": k, "min_s": round(min(v), 2), "median_s": round(statistics.median(v), 2)}
            for k, v in agg.items()]


def download_then_query(repeats):
    """Fetch the bytes with the extension, write them locally, then scan."""
    rows = []
    for _ in range(repeats):
        with tempfile.TemporaryDirectory() as td:
            local = Path(td) / "w.parquet"
            t0 = time.perf_counter()
            # COPY the remote parquet out to a local file, then scan it. Two
            # statements, one process -- which is the generous reading of
            # "download then query": a real user runs two commands.
            subprocess.run([str(DUCKDB), "-noheader", "-list"], text=True,
                           capture_output=True, timeout=1800,
                           input="\n".join([
                               secret_sql(),
                               f"COPY (SELECT * FROM read_parquet('{FIXTURE}')) "
                               f"TO '{local}' (FORMAT parquet);",
                               SCAN.format(uri=str(local)) + ";",
                           ]))
            rows.append(time.perf_counter() - t0)
    return {"op": "Download, then query locally", "min_s": round(min(rows), 2),
            "median_s": round(statistics.median(rows), 2)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--only", default="all")
    ap.add_argument("--out", default=str(REPO / "docs/paper/data/bench_paper.json"))
    args = ap.parse_args()

    if not DUCKDB.is_file():
        print(f"FAIL: {DUCKDB} not built.", file=sys.stderr)
        return 2
    missing = [v for v in REQUIRED if not os.environ.get(v)]
    if missing:
        print(f"FAIL: {', '.join(missing)} not set.", file=sys.stderr)
        return 2

    run_id = f"run-paper-{uuid.uuid4().hex[:8]}"
    out = {"run_id": run_id, "repeats": args.repeats}
    want = args.only

    if want in ("all", "threads"):
        print("==> threads sweep", file=sys.stderr)
        out["threads"] = threads_sweep(args.repeats)
    if want in ("all", "write"):
        print(f"==> write path (gdrive://scratch/{run_id}/)", file=sys.stderr)
        out["write"] = write_path(args.repeats, run_id)
    if want in ("all", "download"):
        print("==> download-then-query", file=sys.stderr)
        out["download"] = download_then_query(args.repeats)
    if want in ("all", "ducklake"):
        print(f"==> ducklake ops (gdrive://scratch/{run_id}/)", file=sys.stderr)
        out["ducklake"] = ducklake_ops(args.repeats, run_id)

    Path(args.out).write_text(json.dumps(out, indent=2) + "\n")
    print(f"\nwrote {args.out}", file=sys.stderr)
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
