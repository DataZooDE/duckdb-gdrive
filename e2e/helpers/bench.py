"""REQ-NF-01: cold 100 MB Parquet scan, gdrive:// vs gs:// vs local.

    make bench

The gate (plan S-4.2) is **within 3x GCS** on the same machine, same file,
same query. If it misses, the fix is fewer round trips -- prefetch and
coalesce adjacent ranges -- not a relaxed gate.

Three legs, and each is here for a reason:

``local``
    The floor. Isolates decode cost from transfer cost, so a slow result can
    be attributed rather than guessed at. Without it, a 3x miss is unreadable:
    you cannot tell a chatty filesystem from an expensive query.
``gs``
    The gate's denominator. Requires ``GDRIVE_BENCH_GCS_URI`` pointing at the
    SAME parquet file in a bucket you can read. Absent, the run still produces
    the other two legs and reports the gate as NOT EVALUATED -- it never
    invents a denominator, because a benchmark that quietly drops its
    comparison is worse than no benchmark.
``gdrive``
    The subject.

**Cold means cold.** Every leg runs in a fresh ``duckdb`` process against a
fresh in-memory database, and the gdrive leg additionally gets a fresh secret,
so no path-resolution cache and no HTTP connection survives from the previous
leg. Warm numbers would be flattering and meaningless -- the thing users feel
is the first scan.

Each leg is timed ``REPEATS`` times and reported by MINIMUM, not mean. On a
shared machine the minimum is the closest thing to the true cost; a mean
mostly measures whatever else the box was doing.
"""

from __future__ import annotations

import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from .drive import Drive, DriveConfigError

REPO_ROOT = Path(__file__).resolve().parents[2]
DUCKDB = REPO_ROOT / "build" / "release" / "duckdb"

#: Timed runs per leg. Three is enough for a minimum to be stable and keeps a
#: 100 MB cold scan over Drive inside a sane wall-clock.
REPEATS = 3

#: The benchmark query. Deliberately NOT `count(*)`: Parquet answers that from
#: the footer without reading a single column chunk, which would measure
#: nothing but metadata latency and flatter every remote filesystem enormously.
#: This touches real column data -- three columns of it.
#:
#: Every aggregate is EXACT. `sum(d1)` over the DOUBLE column was the obvious
#: choice and it is wrong: DuckDB aggregates in parallel, so the summation
#: order varies run to run and the last digit moves. The cross-run identity
#: check below then fires on a benchmark that is working perfectly.
QUERY = (
    "SELECT count(*), sum(id), sum(h1::HUGEINT), min(s1), max(s1) "
    "FROM read_parquet('{uri}')"
)

FIXTURE = "fixtures/wide.parquet"


class BenchError(RuntimeError):
    pass


def _run_duckdb(setup_sql: list[str], query: str) -> tuple[float, str]:
    """Run one cold scan in a FRESH duckdb process. Returns (seconds, output).

    The timing brackets the whole process, including start-up and extension
    load. That is a constant across all three legs (measured by the local leg)
    and it is also what a user actually waits for.
    """
    script = "\n".join(s.rstrip(";") + ";" for s in setup_sql + [query])
    started = time.perf_counter()
    proc = subprocess.run(
        [str(DUCKDB), "-noheader", "-list", "-c", script],
        capture_output=True, text=True,
    )
    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        raise BenchError(
            f"duckdb exited {proc.returncode}\n"
            f"--- stderr ---\n{proc.stderr.strip()}\n"
            f"--- stdout ---\n{proc.stdout.strip()}"
        )
    # Only the LAST line is the query's answer. `CREATE SECRET` prints "true",
    # so the gdrive leg's raw stdout differs from the local leg's for a reason
    # that has nothing to do with the data -- which would make any cross-leg
    # comparison of the recorded result meaningless.
    lines = [ln for ln in proc.stdout.splitlines() if ln.strip()]
    return elapsed, (lines[-1] if lines else "")


def _time_leg(name: str, setup_sql: list[str], uri: str) -> dict:
    query = QUERY.format(uri=uri)
    samples: list[float] = []
    result = ""
    for i in range(REPEATS):
        secs, out = _run_duckdb(setup_sql, query)
        samples.append(secs)
        # Every repeat must return the SAME answer. A leg reading a different
        # file, or a truncated one, would otherwise post a fast time and look
        # like a win.
        if result and out != result:
            raise BenchError(
                f"{name}: run {i} returned {out!r}, run 0 returned {result!r} -- "
                "the legs are not reading the same data"
            )
        result = out
        print(f"    run {i + 1}/{REPEATS}: {secs:.2f}s")
    return {
        "leg": name,
        "uri": uri,
        "min_s": min(samples),
        "median_s": statistics.median(samples),
        "samples_s": [round(s, 3) for s in samples],
        "result": result,
    }


def _local_copy(drive: Drive, file_id: str, dest: Path) -> int:
    """Download the fixture once, so the local leg reads the identical bytes."""
    data = drive.download(file_id)
    dest.write_bytes(data)
    return len(data)


def _gdrive_setup(key_file: str, drive: Drive) -> list[str]:
    binding = (
        f"DRIVE_ID '{drive.drive_id}'" if drive.is_shared_drive
        else f"ROOT_FOLDER_ID '{drive.drive_id}'"
    )
    return [
        "SET autoinstall_known_extensions=false",
        "SET autoload_known_extensions=false",
        # DRIVE_SCOPE, never SCOPE -- SCOPE is a reserved DuckDB clause meaning
        # "which paths may use this secret", and DuckDB's parser eats it before
        # the extension sees it. The secret then matches no gdrive:// path and
        # every query fails with "no secret configured".
        "CREATE SECRET bench (TYPE gdrive, PROVIDER service_account, "
        f"KEY_FILE '{key_file}', {binding}, "
        "DRIVE_SCOPE 'https://www.googleapis.com/auth/drive.readonly')",
    ]


def main() -> int:
    if not DUCKDB.is_file():
        print(f"FAIL: {DUCKDB} not built. Run `make` first.", file=sys.stderr)
        return 2

    try:
        drive = Drive.from_env()
    except DriveConfigError as e:
        print(f"cannot benchmark: {e}", file=sys.stderr)
        return 2

    key_file = os.environ.get("GDRIVE_CI_SA_KEY_FILE", "")
    if not key_file:
        print("FAIL: GDRIVE_CI_SA_KEY_FILE must point at the service-account key.",
              file=sys.stderr)
        return 2
    key_file = str(Path(key_file).resolve())

    gcs_uri = os.environ.get("GDRIVE_BENCH_GCS_URI", "").strip()

    print(f"==> resolving {FIXTURE}")
    file_id = drive.resolve_path(FIXTURE)
    meta = drive.get_metadata(file_id)
    size_mb = int(meta.get("size", 0)) / 1e6
    print(f"    {file_id}  ({size_mb:.1f} MB)")

    legs: list[dict] = []
    with tempfile.TemporaryDirectory() as td:
        local_path = Path(td) / "wide.parquet"
        print("==> downloading a local copy (identical bytes, for the floor leg)")
        got = _local_copy(drive, file_id, local_path)
        print(f"    {got / 1e6:.1f} MB -> {local_path}")

        print("==> leg: local")
        legs.append(_time_leg("local", ["SET autoinstall_known_extensions=false"],
                              str(local_path)))

        if gcs_uri:
            print(f"==> leg: gs ({gcs_uri})")
            legs.append(_time_leg("gs", ["INSTALL httpfs", "LOAD httpfs"], gcs_uri))
        else:
            print("==> leg: gs SKIPPED -- GDRIVE_BENCH_GCS_URI is not set")

        print("==> leg: gdrive")
        legs.append(_time_leg("gdrive", _gdrive_setup(key_file, drive),
                              f"gdrive://id:{file_id}"))

    # Every leg must have computed the SAME answer. This is what makes the
    # ratio meaningful: without it, a gs:// leg pointed at a different (say,
    # smaller) file would post a fast time and the gate would "pass" on a
    # comparison that never happened.
    answers = {leg["leg"]: leg["result"] for leg in legs}
    if len(set(answers.values())) != 1:
        print("\nFAIL: the legs did not read the same data:", file=sys.stderr)
        for name, ans in answers.items():
            print(f"  {name:<7} {ans}", file=sys.stderr)
        return 1

    by_name = {leg["leg"]: leg for leg in legs}
    gdrive_s = by_name["gdrive"]["min_s"]

    print()
    print("  leg      min       median    ")
    print("  -------  --------  --------")
    for leg in legs:
        print(f"  {leg['leg']:<7}  {leg['min_s']:>6.2f}s  {leg['median_s']:>6.2f}s")

    verdict = "NOT EVALUATED"
    ratio = None
    if "gs" in by_name:
        ratio = gdrive_s / by_name["gs"]["min_s"]
        verdict = "PASS" if ratio <= 3.0 else "FAIL"
        print(f"\n  gdrive / gs = {ratio:.2f}x   gate <= 3.00x   {verdict}")
    else:
        print("\n  gate NOT EVALUATED: no gs:// leg. Set GDRIVE_BENCH_GCS_URI to")
        print("  the same parquet file in a bucket you can read.")

    report = {
        "fixture": FIXTURE,
        "file_id": file_id,
        "size_bytes": int(meta.get("size", 0)),
        "repeats": REPEATS,
        "query": QUERY,
        "legs": legs,
        "gdrive_over_gs": ratio,
        "gate": {"limit": 3.0, "verdict": verdict},
    }
    out = REPO_ROOT / "docs" / "benchmark.json"
    out.write_text(json.dumps(report, indent=2) + "\n")
    print(f"\n  raw numbers -> {out.relative_to(REPO_ROOT)}")

    # A missed gate is a red run, not a footnote. NOT EVALUATED is not a pass
    # either -- it exits non-zero so CI cannot go green on a benchmark that
    # never actually compared anything.
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
