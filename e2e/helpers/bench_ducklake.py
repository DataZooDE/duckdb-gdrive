"""DuckLake end to end, same workload, DATA_PATH on three storage backends.

    make bench_ducklake

The catalog is a local DuckDB file in every configuration; only the data path
moves. That is the supported shape -- a DuckLake catalog needs atomic renames
and Drive has none.

Each operation runs in its OWN duckdb process against a shared, persistent
catalog, in sequence. That is what makes "scan, cold" actually cold: reusing
one process would serve it from a warm path cache, a live TLS connection and
DuckDB's external file cache, and the number would describe nothing a user
experiences.

CACHING. DuckLake at the version pinned below exposes no local data-file cache
of its own -- the only relevant knob is DuckDB core's
`enable_external_file_cache`, an in-memory cache of external file data, on by
default. The separate `duckdb-diskcache` extension is a different mechanism
and is not what is measured here. Settings are enumerated from
duckdb_settings() rather than quoted, because this surface has moved between
releases.
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

STEPS = [
    ("CREATE TABLE AS", "CREATE TABLE t AS SELECT i AS id, i%7 AS grp, "
                        "md5(i::VARCHAR) AS s FROM range(200000) tt(i)"),
    ("INSERT (append)", "INSERT INTO t SELECT i, i%7, md5(i::VARCHAR) "
                        "FROM range(200000,250000) tt(i)"),
    ("DELETE", "DELETE FROM t WHERE grp = 3"),
    ("Scan, cold", "SELECT count(*), sum(id) FROM t"),
    ("Time-travel read", "SELECT count(*) FROM t AT (VERSION => 1)"),
]


def gdrive_secret() -> str:
    return ("CREATE SECRET b (TYPE gdrive, PROVIDER config,"
            f" CLIENT_ID '{os.environ['GDRIVE_OAUTH_CLIENT_ID']}',"
            f" CLIENT_SECRET '{os.environ['GDRIVE_OAUTH_CLIENT_SECRET']}',"
            f" REFRESH_TOKEN '{os.environ['GDRIVE_USER_REFRESH_TOKEN']}',"
            f" ROOT_FOLDER_ID '{os.environ['GDRIVE_CI_DRIVE_ID']}',"
            " DRIVE_SCOPE 'https://www.googleapis.com/auth/drive')")


def gcs_secret() -> str:
    tok = subprocess.run(["gcloud", "auth", "print-access-token"],
                         capture_output=True, text=True, timeout=120,
                         check=True).stdout.strip()
    if not tok:
        raise RuntimeError("empty gcloud token")
    return f"CREATE SECRET g (TYPE GCS, BEARER_TOKEN '{tok}')"


def s3_secret() -> str:
    blob = subprocess.run(["aws", "configure", "export-credentials",
                           "--format", "process"],
                          capture_output=True, text=True, timeout=120,
                          check=True).stdout
    c = json.loads(blob)
    region = (os.environ.get("AWS_REGION")
              or subprocess.run(["aws", "configure", "get", "region"],
                                capture_output=True, text=True).stdout.strip())
    parts = [f"KEY_ID '{c['AccessKeyId']}'", f"SECRET '{c['SecretAccessKey']}'",
             f"REGION '{region}'"]
    if c.get("SessionToken"):
        parts.append(f"SESSION_TOKEN '{c['SessionToken']}'")
    return f"CREATE SECRET s (TYPE S3, {', '.join(parts)})"


def prelude(backend: str) -> list[str]:
    base = ["INSTALL ducklake", "LOAD ducklake"]
    if backend == "gdrive":
        return base + [gdrive_secret()]
    base = ["INSTALL httpfs", "LOAD httpfs"] + base
    return base + [gcs_secret() if backend == "gs" else s3_secret()]


def run(statements: list[str]) -> float:
    script = "\n".join(s.rstrip(";") + ";" for s in statements)
    t0 = time.perf_counter()
    p = subprocess.run([str(DUCKDB), "-noheader", "-list"], input=script,
                       capture_output=True, text=True, timeout=2400)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        raise RuntimeError(f"exit {p.returncode}\n{p.stdout[-1200:]}{p.stderr[-1200:]}")
    return dt


def one_backend(backend: str, data_root: str, repeats: int, cache: bool):
    per_op: dict[str, list[float]] = {}
    for i in range(repeats):
        cat = Path(tempfile.mkdtemp(prefix=f"dl-{backend}-")) / "lake.db"
        head = prelude(backend) + [
            f"SET enable_external_file_cache={'true' if cache else 'false'}",
            f"ATTACH 'ducklake:{cat}' AS lake (DATA_PATH '{data_root}/i{i}/')",
            "USE lake",
        ]
        # No replay: the catalog is a persistent file, so each process picks
        # up where the last left off. Replaying would re-run the CTAS against a
        # catalog that already has the table. One fresh process per operation
        # is also exactly what makes "scan, cold" cold.
        for name, sql in STEPS:
            dt = run(head + [sql])
            per_op.setdefault(name, []).append(dt)
            print(f"  [{backend} {i} cache={cache}] {name:<18} {dt:6.2f}s",
                  file=sys.stderr, flush=True)
    return {k: {"min_s": round(min(v), 2), "median_s": round(statistics.median(v), 2)}
            for k, v in per_op.items()}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int, default=2)
    ap.add_argument("--backends", default="gdrive,gs,s3")
    ap.add_argument("--cache-sweep", action="store_true",
                    help="also run the scan with enable_external_file_cache off")
    ap.add_argument("--out", default=str(REPO / "docs/paper/data/bench_ducklake.json"))
    args = ap.parse_args()

    run_id = f"run-dl-{uuid.uuid4().hex[:8]}"
    roots = {
        "gdrive": f"gdrive://scratch/{run_id}",
        "gs": f"gs://{os.environ.get('GDRIVE_BENCH_GCS_BUCKET','')}/{run_id}",
        "s3": f"s3://{os.environ.get('GDRIVE_BENCH_S3_BUCKET','')}/{run_id}",
    }
    out = {"run_id": run_id, "repeats": args.repeats,
           "ducklake_version": "d8a1881e", "duckdb": "v1.5.5", "results": {}}

    for b in args.backends.split(","):
        print(f"==> {b}  ({roots[b]})", file=sys.stderr)
        out["results"][b] = {"cache_on": one_backend(b, roots[b], args.repeats, True)}
        if args.cache_sweep:
            out["results"][b]["cache_off"] = one_backend(b, roots[b] + "-nc",
                                                         args.repeats, False)

    Path(args.out).write_text(json.dumps(out, indent=2) + "\n")
    print(f"\nwrote {args.out}", file=sys.stderr)
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
