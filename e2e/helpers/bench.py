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
#: Overridable, because three was not enough. The gdrive leg is stable to
#: ~9% but the gs leg swung 1.03-1.72 s across three full runs, and the 3x
#: verdict flipped from FAIL to PASS on that alone -- the DENOMINATOR's noise,
#: not any change in the subject. A gate decided by noise is not a gate.
REPEATS = int(os.environ.get("GDRIVE_BENCH_REPEATS", "3"))

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
    # Script on STDIN, deliberately, not `-c`. The gs leg's setup carries a
    # GCS bearer token, and argv is world-readable through /proc -- `ps` on a
    # shared box would show it. REQ-NF-03 is about the extension, but a
    # benchmark that leaks a credential is still a leaked credential.
    proc = subprocess.run(
        [str(DUCKDB), "-noheader", "-list"],
        input=script, capture_output=True, text=True,
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


def _gcs_setup() -> list[str]:
    """Setup SQL for the gs:// leg, including credentials.

    The first version of this leg was `INSTALL httpfs; LOAD httpfs` and
    nothing else, which works only for a PUBLIC object. Against a private
    bucket it fails with "No credentials provided" -- so the gate's
    denominator could never be measured against a bucket you would actually
    create for the purpose.

    A short-lived OAuth2 bearer token from the ambient gcloud login, rather
    than HMAC keys: nothing durable is minted, nothing is written to disk, and
    it expires on its own within the hour. GDRIVE_BENCH_GCS_BEARER overrides
    it for environments with no gcloud.

    The token is never printed, never returned in the result dict, and never
    reaches benchmark.json -- only this list, which goes straight to duckdb's
    stdin.
    """
    token = os.environ.get("GDRIVE_BENCH_GCS_BEARER", "").strip()
    if not token:
        try:
            token = subprocess.run(
                ["gcloud", "auth", "print-access-token"],
                capture_output=True, text=True, timeout=120, check=True,
            ).stdout.strip()
        except Exception as e:
            raise BenchError(
                "the gs:// leg needs credentials and none are available: "
                f"`gcloud auth print-access-token` failed ({type(e).__name__}). "
                "Run `gcloud auth login`, or set GDRIVE_BENCH_GCS_BEARER."
            ) from e
    if not token:
        raise BenchError("gcloud returned an empty access token")
    return [
        "INSTALL httpfs", "LOAD httpfs",
        f"CREATE SECRET gcs_bench (TYPE GCS, BEARER_TOKEN '{token}')",
    ]


def _s3_setup() -> list[str]:
    """Setup SQL for the s3:// leg, including credentials.

    Same discipline as the gs:// leg: short-lived credentials from the ambient
    login, nothing durable minted, nothing written to disk, and the values go
    to duckdb's stdin rather than argv.

    `aws configure export-credentials` resolves whatever the caller is using --
    SSO, an assumed role, a profile, an instance role -- and hands back a
    concrete triple. An assumed role yields a SESSION_TOKEN, which DuckDB's s3
    secret needs and which is easy to forget: without it the request is signed
    with a key that only exists inside a session, and S3 answers
    `InvalidAccessKeyId`, which reads like a typo rather than a missing field.

    Deliberately NOT `PROVIDER credential_chain`: that needs the `aws`
    extension, which is another download inside a timed benchmark leg.
    """
    try:
        blob = subprocess.run(
            ["aws", "configure", "export-credentials", "--format", "process"],
            capture_output=True, text=True, timeout=120, check=True,
        ).stdout
    except Exception as e:
        raise BenchError(
            "the s3:// leg needs credentials and none are available: "
            f"`aws configure export-credentials` failed ({type(e).__name__}). "
            "Run `aws sso login` (or set AWS_PROFILE), or unset "
            "GDRIVE_BENCH_S3_URI to skip the leg."
        ) from e
    try:
        creds = json.loads(blob)
    except json.JSONDecodeError as e:
        raise BenchError("aws returned no parseable credentials") from e

    region = (os.environ.get("AWS_REGION")
              or os.environ.get("AWS_DEFAULT_REGION")
              or subprocess.run(["aws", "configure", "get", "region"],
                                capture_output=True, text=True).stdout.strip())
    if not region:
        raise BenchError("no AWS region: set AWS_REGION or `aws configure set region`")

    parts = [f"KEY_ID '{creds['AccessKeyId']}'",
             f"SECRET '{creds['SecretAccessKey']}'",
             f"REGION '{region}'"]
    if creds.get("SessionToken"):
        parts.append(f"SESSION_TOKEN '{creds['SessionToken']}'")
    return [
        "INSTALL httpfs", "LOAD httpfs",
        f"CREATE SECRET s3_bench (TYPE S3, {', '.join(parts)})",
    ]


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
    s3_uri = os.environ.get("GDRIVE_BENCH_S3_URI", "").strip()

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
            legs.append(_time_leg("gs", _gcs_setup(), gcs_uri))
        else:
            print("==> leg: gs SKIPPED -- GDRIVE_BENCH_GCS_URI is not set")

        # S3 is a second object-store reference point, not a second gate. The
        # gate is against GCS and stays there; adding a leg is not a licence to
        # pick whichever denominator flatters the result.
        if s3_uri:
            print(f"==> leg: s3 ({s3_uri})")
            legs.append(_time_leg("s3", _s3_setup(), s3_uri))
        else:
            print("==> leg: s3 SKIPPED -- GDRIVE_BENCH_S3_URI is not set")

        print("==> leg: gdrive")
        legs.append(_time_leg("gdrive", _gdrive_setup(key_file, drive),
                              f"gdrive://id:{file_id}"))

        # The same query with gdrive_block_size_bytes at 128 MiB -- the
        # configuration docs/benchmark.md documents for large sequential
        # scans. Measured here, in the SAME session and the same methodology,
        # because the alternative is quoting a tuned number from one harness
        # against a denominator from another, which is not a measurement.
        #
        # It does NOT decide the gate. The gate is about what a user gets out
        # of the box, and out of the box this is 16 MiB.
        print("==> leg: gdrive_tuned (128 MiB blocks)")
        legs.append(_time_leg(
            "gdrive_tuned",
            _gdrive_setup(key_file, drive) + [
                f"SET gdrive_block_size_bytes={128 * 1024 * 1024}"],
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
        gs_s = by_name["gs"]["min_s"]
        ratio = gdrive_s / gs_s
        verdict = "PASS" if ratio <= 3.0 else "FAIL"
        print(f"\n  gdrive / gs = {ratio:.2f}x   gate <= 3.00x   {verdict}")
        if "gdrive_tuned" in by_name:
            tuned = by_name["gdrive_tuned"]["min_s"] / gs_s
            print(f"  tuned  / gs = {tuned:.2f}x   (128 MiB blocks; "
                  f"informational, not the gate)")
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
