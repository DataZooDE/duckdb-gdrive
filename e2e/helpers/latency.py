"""Per-request latency of the Drive API endpoints this extension actually uses.

    make latency

Why this exists: the README, the BRD and the community-extensions descriptor
all claimed "a Drive API round trip is ~150 ms". Nobody measured that. When
the media endpoint WAS measured (docs/benchmark.md, the block-size sweep) it
showed a ~1.2 s floor -- roughly 8x the published figure, in the flattering
direction -- and benchmark.md ended up contradicting itself in two places.

The claim is about "a Drive API round trip" in general, not about media, so
correcting it needs all three endpoints measured, not one:

``files.list``   one per path segment during resolution -- the R-1 cost
``files.get``    metadata, one per open
``alt=media``    the bytes, ranged

**Warm keep-alive, deliberately.** A fresh TLS handshake per request would
measure the handshake, and the extension holds a thread_local SSLClient
precisely so it does not pay that. This uses one ``requests.Session`` for the
same reason. The number we publish should be the one a running query sees.

Reported by MEDIAN. The minimum is right for the benchmark (it estimates the
true cost of a whole scan on a shared box); for a per-request figure quoted
in prose, the median is what a user actually experiences, and the spread
matters as much as the centre -- so p0/p50/p90 are all printed.
"""

from __future__ import annotations

import os
import statistics
import sys
import time

import google.auth.transport.requests as gauth_requests
import requests

from .drive import Drive, DriveConfigError
from . import fixtures as fx

#: Enough to be stable, few enough to stay polite against a shared quota.
REPEATS = 10

API = "https://www.googleapis.com/drive/v3"
SHARED = {"supportsAllDrives": "true", "includeItemsFromAllDrives": "true"}


def _percentiles(samples: list[float]) -> dict:
    ordered = sorted(samples)
    return {
        "n": len(ordered),
        "min_ms": round(ordered[0] * 1000),
        "p50_ms": round(statistics.median(ordered) * 1000),
        "p90_ms": round(ordered[min(len(ordered) - 1, int(len(ordered) * 0.9))] * 1000),
    }


def _time(session: requests.Session, label: str, fn, repeats: int = REPEATS) -> dict:
    # One untimed call first. The very first request on a Session pays TCP +
    # TLS, which is exactly the cost this probe is meant to exclude -- leave
    # it in and every endpoint's minimum is really a handshake measurement.
    fn(session)
    samples = []
    for _ in range(repeats):
        start = time.perf_counter()
        resp = fn(session)
        resp.raise_for_status()
        # Consume the body: requests streams, and a media response that is
        # never read would time the headers only.
        _ = len(resp.content)
        samples.append(time.perf_counter() - start)
    result = _percentiles(samples)
    result["endpoint"] = label
    return result


def main() -> int:
    try:
        drive = Drive.from_env()
    except DriveConfigError as e:
        print(f"SKIP: {e}", file=sys.stderr)
        return 0

    root = os.environ["GDRIVE_CI_DRIVE_ID"]
    small_id = drive.resolve_path(f"{fx.FIXTURES_ROOT}/small.csv")

    token = drive._creds
    if not token.valid:
        token.refresh(gauth_requests.Request())
    session = requests.Session()
    session.headers["Authorization"] = f"Bearer {token.token}"

    rows = [
        _time(session, "files.list (one path segment)", lambda s: s.get(
            f"{API}/files",
            params={**SHARED, "q": f"'{root}' in parents and name = '{fx.FIXTURES_ROOT}'",
                    "fields": "files(id,name)"},
            timeout=60)),
        _time(session, "files.get (metadata)", lambda s: s.get(
            f"{API}/files/{small_id}",
            params={**SHARED, "fields": "id,name,size,mimeType,headRevisionId"},
            timeout=60)),
        _time(session, "files.get?alt=media (small body)", lambda s: s.get(
            f"{API}/files/{small_id}",
            params={**SHARED, "alt": "media"}, timeout=60)),
    ]

    # A ranged read of the big fixture: the request shape a Parquet scan
    # actually issues, and the one the 3x gate turns on.
    #
    # This is the shape that matters, and the shape docs/benchmark.md's "~1.2 s
    # floor" table actually measured. A ranged read of a LARGE object is not
    # the same request as a whole read of a small one -- conflating them is
    # how that table and the small-body row above came to disagree by 2x.
    try:
        big_id = drive.resolve_path("fixtures/wide.parquet")
    except Exception as e:
        print(f"NOTE: no fixtures/wide.parquet ({e}); ranged rows skipped.",
              file=sys.stderr)
        big_id = None
    if big_id:
        for label, upper in (("1 KiB", 1023), ("1 MiB", 1048575), ("16 MiB", 16777215)):
            rows.append(_time(
                session, f"files.get?alt=media ({label} range of 87 MB)",
                lambda s, u=upper: s.get(
                    f"{API}/files/{big_id}",
                    params={**SHARED, "alt": "media"},
                    headers={"Range": f"bytes=0-{u}"}, timeout=300),
                repeats=5))

    width = max(len(r["endpoint"]) for r in rows)
    print(f"\n{'endpoint'.ljust(width)}   n   min     p50     p90")
    for r in rows:
        print(f"{r['endpoint'].ljust(width)}  {r['n']:>2}  "
              f"{r['min_ms']:>5} ms {r['p50_ms']:>5} ms {r['p90_ms']:>5} ms")

    slowest = max(r["p50_ms"] for r in rows)
    fastest = min(r["p50_ms"] for r in rows)
    print(f"\nper-request p50 across endpoints: {fastest}-{slowest} ms")
    print("Quote the RANGE, not one endpoint: path resolution and byte fetch")
    print("differ, and a single figure is what produced the ~150 ms claim.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
