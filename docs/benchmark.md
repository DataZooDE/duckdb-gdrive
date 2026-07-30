# REQ-NF-01 — cold Parquet scan

**Gate:** a cold columnar scan over `gdrive://` completes within **3×** the
same file on Google Cloud Storage.

**Status: UNSETTLED, and borderline.** The 4.8x figure below was measured on
2026-07-27, before the shared block cache. The gdrive leg has since dropped
from 6.21 s to **4.36 s** (measured 2026-07-30), which against the last
recorded GCS minimum would be ~3.4x — still a miss, but no longer a
comfortable one, and the fastest measured configuration (128 MiB blocks +
`id:` form) is ~2.6x, inside the gate.

The gate cannot be called from here, because **both legs must run in one
session** and the benchmark bucket was deleted after the last run. Until then
this document reports the numerator honestly and does not assert a verdict.
The cause of the cost is measured, not guessed — 55 Drive round trips for one
query — and the fix is fewer of them, not a softer gate. See *Diagnosis*.

Reproduce with `make bench` (needs `GDRIVE_BENCH_GCS_URI`); raw numbers land
in `docs/benchmark.json`.

## Method

One 87 MB zstd-compressed Parquet file (2,000,000 rows × 6 columns), the same
bytes in every leg, scanned with:

```sql
SELECT count(*), sum(id), sum(h1::HUGEINT), min(s1), max(s1)
FROM read_parquet('<uri>')
```

Three columns of six are touched, which is what a real analytical scan does.
Deliberately **not** `count(*)` alone — Parquet answers that from the footer
without reading a single column chunk, which would measure metadata latency
and flatter every remote filesystem enormously.

Every aggregate is exact-integer. `sum()` over a DOUBLE column varies in its
last digit between runs because DuckDB aggregates in parallel, which would
break the identity check below for a reason unrelated to the filesystem.

**Cold means cold.** Each timed run is a fresh `duckdb` process against a
fresh in-memory database, and the `gdrive` leg additionally builds a fresh
secret — so no path-resolution cache and no HTTP connection survives from the
previous run. Warm numbers would be flattering and would not describe what a
user feels, which is the first scan.

Three runs per leg, reported by **minimum**. On a shared machine the minimum
is the closest available estimate of the true cost; a mean mostly measures
whatever else the box was doing. All legs must return an identical result or
the run fails — otherwise a leg pointed at the wrong file would post a fast
time and "pass".

## Results

Measured 2026-07-27. Linux 6.18, **DuckDB v1.5.5**, extension statically
linked. Fixture: `/fixtures/wide.parquet` (87.0 MB), the identical bytes in
all three legs.

| Leg | min | median | vs local |
|---|---|---|---|
| `local` | 0.14 s | 0.14 s | 1.0x |
| `gs` (GCS) | 1.30 s | 1.39 s | 9.5x |
| `gdrive` | 6.21 s | 6.77 s | 45.4x |

## **gdrive / GCS = 4.79x — the 3x gate FAILS**

Two independent runs gave 5.68x and 4.79x. This is not a marginal miss and
not sampling noise: even taking the fastest `gdrive` minimum ever recorded
(4.43 s, before this fixture was re-generated) against the slowest `gs`
minimum (1.30 s), the ratio is 3.4x.

**REQ-NF-01 is therefore NOT MET, and BRD success criterion 2 is not met.**

### Method note

The GCS leg reads the object over `https://storage.googleapis.com/...`
rather than `gs://`. DuckDB's `gcs` secret type supports HMAC keys only, and
creating a long-lived credential (or making a bucket permanently public) for
a benchmark was not a reasonable trade. It is the same GCS backend, the same
bytes and the same network path, so it measures what the gate asks about.
The bucket was deleted immediately after measurement.

## Diagnosis

The same query through this extension issues **55 Drive API round trips**:

| Call | Count |
|---|---|
| `files.get` (metadata) | 18 |
| `files.list` (path resolution) | 2 |
| `files.get?alt=media` (data) | 35 |

At the ~150 ms per round trip assumed at the time that is roughly 8 s of pure
latency, which is essentially the whole measurement. GCS moves the same bytes
in ~1.2 s. (The 150 ms was itself wrong — see *Per-request latency* below. The
arithmetic here is left as it was written because the conclusion it led to,
"the request COUNT is the problem", survived the correction; the real figures
make the case more strongly, not less.)

Two things stand out, and neither is "Drive is slow":

1. **18 metadata calls for one file.** A single scan should need one. This
   looks like a per-open metadata refresh multiplied by however many times
   DuckDB opens the handle for a parallel scan. Almost certainly avoidable.
2. **35 data requests, uncoalesced.** Adjacent or near-adjacent column
   chunks are fetched as separate ranged GETs. Coalescing them trades a
   little wasted bandwidth for a large latency saving — the right trade when
   a ranged media request costs ~1.3 s regardless of how few bytes it asks
   for.

Plan section S-4.2 anticipated exactly this: *"If it misses, the fix is fewer
round trips (prefetch/coalesce adjacent ranges), not a relaxed gate."* That
remains the correct response. The gate stays at 3x.

## Reproducing

The `gs://` leg needs the same Parquet file readable by DuckDB. Note that
DuckDB's `gcs` secret type supports HMAC keys only — there is no credential
chain — so either create an HMAC key, or (as here) put the object behind a
public HTTPS URL in a throwaway bucket and delete it afterwards:

```bash
gcloud storage buckets create gs://<bucket> --location=EU
gcloud storage cp wide.parquet gs://<bucket>/wide.parquet
gcloud storage buckets add-iam-policy-binding gs://<bucket> \
    --member=allUsers --role=roles/storage.objectViewer
GDRIVE_BENCH_GCS_URI=https://storage.googleapis.com/<bucket>/wide.parquet make bench
gcloud storage rm -r gs://<bucket>          # do not leave it public
```

`make bench` exits **non-zero** both when the leg is absent and when the gate
is missed. A benchmark that omits its own denominator and prints a green line
is worse than no benchmark, so "not evaluated" is a failure state here, not a
footnote.

## If the gate is missed

Per plan §S-4.2 the fix is **fewer round trips** — prefetching and coalescing
adjacent byte ranges so a Parquet scan issues fewer, larger requests — not a
relaxed gate. Drive's per-request latency dominates (see *Per-request latency*
below: 0.3–2.1 s depending on the request shape), so request count is the
lever that matters.

## Where the time actually goes (measured 2026-07-27)

The first diagnosis above — "55 round trips at ~150 ms" — was directionally
right and quantitatively wrong. Measuring properly changed the conclusion.
Where the ~150 ms came from is not recorded anywhere; it appears to have been
assumed. It was published in the README, the BRD and the community-extensions
descriptor for as long as it existed. See *Per-request latency* below for what
it actually is.

### Drive's media endpoint has a ~1.2 s floor per request

On a warm keep-alive connection, timed with `requests.Session`:

| Request | Time |
|---|---|
| 1 KB | 1.21 s |
| 1 KB (second) | 1.37 s |
| 1 MB | 1.03 s |
| 4 MB | 1.42 s |
| 16 MB | 1.60 s |
| **87 MB (whole file)** | **2.06 s** |

A 1 KB read costs about as much as a 1 MB read. The marginal transfer rate is
roughly 160 MB/s; the fixed cost is ~1.0–1.2 s and dominates everything below
about 100 MB. This is Drive-side, not ours: it is the same on a warm socket
with no handshake.

**The consequence is counterintuitive and it drives the design: on Drive,
fetching MORE data in FEWER requests is faster.** One 87 MB request (2.06 s)
beats 35 ranged requests totalling 54.9 MB (4.94 s) by 2.4x.

### Per-request latency, by endpoint and request shape (measured 2026-07-30)

`make latency` (`e2e/helpers/latency.py`), warm keep-alive `requests.Session`,
service-account identity, p50 of 5–10 samples:

| Request | min | **p50** | p90 |
|---|---|---|---|
| `files.list` (one path segment) | 291 ms | **330 ms** | 486 ms |
| `files.get` (metadata) | 263 ms | **293 ms** | 326 ms |
| `files.get?alt=media`, whole 100-byte file | 509 ms | **569 ms** | 730 ms |
| `files.get?alt=media`, 1 KiB range of the 87 MB file | 1223 ms | **1348 ms** | 2491 ms |
| `files.get?alt=media`, 1 MiB range of the 87 MB file | 1297 ms | **1375 ms** | 2342 ms |
| `files.get?alt=media`, 16 MiB range of the 87 MB file | 1599 ms | **2103 ms** | 2488 ms |

Three things this settles.

**The `~150 ms` figure was wrong by 2–14x, and always in the flattering
direction.** No endpoint is anywhere near it. It is corrected in the README,
`docs/brd.md` and the community-extensions descriptor.

**Request SHAPE matters more than request size.** A 1 KiB range and a 1 MiB
range of the same large object cost the same (1348 vs 1375 ms) — but a whole
read of a *small* file costs 569 ms, less than half. The cost is not "a Drive
round trip"; it is "a ranged read of a large object", and that is the request
a Parquet scan is made of. Quoting one number for both is precisely the
mistake that produced the original claim.

**This reconciles the ~1.2 s floor table below with the 569 ms small-body row
above.** They disagree by 2x and both are correct: the floor table measured
ranged reads of the 87 MB file, the small-body row measures a whole small
file. Neither generalises to "a Drive API round trip".

Metadata calls, at ~300 ms, are the cheap ones — which is why the R-1 path
cache is worth having but was never where the time went.

### What the request pattern actually looks like

Tracing the benchmark query (`GDRIVE_TRACE_RANGES=1`):

* 35 ranged GETs, 54.9 MB of the 87 MB file — so ranged reads do work, and
  the reader is genuinely skipping the columns it does not need.
* Sorted by offset, the gaps between consecutive ranges alternate 1929 KB and
  **0 KB**: half of all requests begin exactly where another ended.
* But those adjacent pairs are on **different file handles** — 35 reads across
  18 handles, and *zero* adjacent pairs share a handle.

That last point killed the obvious fix. A per-handle read-ahead buffer was
implemented and measured: it changed the request count not at all (the
follow-on read is another thread's handle) while inflating every request to
the block size, fetching **136 MB instead of 54.9 MB**. It was reverted. The
measurement is recorded here because "add read-ahead" is the natural first
idea and it is wrong for this access pattern.

### What was fixed

* **Metadata: 19 round trips -> 1.** DuckDB opens a handle per thread and each
  open fetched metadata. Now cached per query. gdrive leg 6.21 s -> 4.94 s.
* **Connection reuse.** The client built a fresh `SSLClient` — new TCP
  connection, full TLS handshake — for every request. Now one connection per
  thread, reused across requests and queries. Median request 1558 ms ->
  1334 ms.

### What would close the gate, and its cost

Given a ~1.2 s per-request floor, the only remaining lever is fewer, larger
requests, and the data says that is worth a lot. The design that follows from
the measurements is a **shared, file-level block cache** — blocks in the tens
of megabytes, shared across all handles for one file, so 18 threads scanning
one file issue a handful of requests rather than 35.

The catch is memory, which is why this is not simply done: a whole-file or
large-block cache holds tens of megabytes per open file, and the natural
"just fetch the whole file" version is unbounded. That is a policy decision
about default memory use, not a pure optimisation, so it is written down here
rather than chosen unilaterally.

Projected: one whole-file fetch is 2.06 s against GCS's 1.30 s = **1.58x**,
comfortably inside the 3x gate.

## Shared block cache — measured (2026-07-27)

Implemented per the finding above: blocks keyed by identity + file id +
headRevisionId + block index, shared across handles, concurrent readers of one
block coalesced onto a single fetch via `shared_future`.

Block-size sweep, best of two, same query, fresh process each time (so wall
time includes ~0.15 s of DuckDB start-up):

| `gdrive_block_size_bytes` | media requests | wall |
|---|---|---|
| 0 (exact ranges) | 35 | 5.32 s |
| 4 MiB | 21 | 5.74 s |
| 8 MiB | 11 | 4.89 s |
| **16 MiB (default)** | **6** | **4.70 s** |
| 32 MiB | 3 | 4.62 s |
| 128 MiB (whole file) | 1 | 4.26 s |
| 128 MiB + `gdrive://id:` form | 1 | **3.42 s** |

Three things this says, none of them obvious beforehand:

1. **Fewer requests help, but far less than the ~1.2 s floor suggests.** The
   35 exact reads were already spread across 18 threads — roughly two per
   thread in sequence — so the floor was being paid twice, not thirty-five
   times. Collapsing to one request removes about a second, not thirty.
2. **4 MiB is WORSE than no cache.** Small blocks add bytes without removing
   enough requests. The curve is not monotonic and guessing a block size
   without measuring would have made things slower.
3. **Path resolution is now a visible cost.** The `id:` form saves 0.84 s on
   an otherwise identical query — two `files.list` calls at the ~1.2 s floor.
   For a cold process this is unavoidable on Drive: the API has no path
   addressing. It is the strongest argument for the documented `id:` fast
   path.

### The sweep above was under-sampled — resampled 2026-07-30

"Best of two" is not enough to separate 4.62 s from 4.70 s. Re-run with five
samples per cell, reporting both min and median, and adding the counterweight
the first sweep never measured: what a **footer-only** query (`count(*)`,
which Parquet answers without reading a column chunk) costs at each size.

| `gdrive_block_size_bytes` | scan min | scan median | `count(*)` min | `count(*)` median |
|---|---|---|---|---|
| **16 MiB (default)** | 5.01 s | **5.43 s** | 2.31 s | **2.61 s** |
| 32 MiB | 4.75 s | 5.47 s | 2.81 s | 2.85 s |
| 64 MiB | 4.69 s | 5.05 s | 2.73 s | 2.82 s |
| 128 MiB | **3.34 s** | **3.49 s** | 3.23 s | **3.40 s** |

(Higher than the table above because each run includes process start-up and
`CREATE SECRET`; the comparison between rows is what matters.)

**32 MiB was noise.** The first sweep's 4.62 s made it look like a free 0.08 s
over the default; at five samples its median is *worse* than 16 MiB and its
footer cost is 0.24 s higher. A default was nearly changed on that. Two
samples cannot distinguish an 80 ms effect on a workload whose median moves by
400 ms between runs.

**128 MiB is a real trade, not a free win, and smaller than previously
believed.** It buys ~1.9 s on a full scan and costs ~0.8 s on a footer query.
The earlier reasoning for keeping 16 MiB — that a large block would make
`count(*)` "download the whole file" — overstated it: the footer read is a
single media request at every block size, so the cost is transferring one
larger block, not the file. The conclusion survives the correction anyway:

**The 16 MiB default stands.** A footer-only query is the cheap thing users
expect to be cheap, and 128 MiB also fits only two blocks inside the 256 MiB
`gdrive_block_cache_bytes` cap, so a multi-file scan would thrash. Users whose
workload is large sequential scans should set `gdrive_block_size_bytes` to
128 MiB and use the `gdrive://id:` form; that combination is the fastest
measured configuration and it is documented rather than defaulted.

### Where that leaves REQ-NF-01

**Provisional — the denominator is stale.** The ratios below divide a fresh
gdrive number by a GCS minimum recorded on 2026-07-27, in a different session,
on a bucket that no longer exists. That is exactly the sloppiness this document
exists to correct, so they are marked as estimates and the gate is **not**
settled until both legs run in one session (`make bench` with
`GDRIVE_BENCH_GCS_URI`).

Current gdrive leg, measured 2026-07-30 by `make bench`: **4.36 s min /
4.80 s median**, down from 6.21 s, entirely from the shared block cache.

Against the stale GCS minimum of 1.30 s:

* default 16 MiB blocks, path form: **~3.4x** — would still miss
* 128 MiB blocks, `id:` form: **~2.6x** — would pass

So the gate is genuinely borderline in the default configuration and the
outcome cannot be asserted either way from here.

So the gate is achievable but NOT met at the defaults, and the honest summary
is that Drive can be brought within 3x of GCS for id-addressed reads of large
files, and not yet for cold path-addressed ones.

The default stays 16 MiB rather than something that games the benchmark: a
128 MiB block means a `count(*)` — which needs only the Parquet footer —
downloads the entire file. That is a bad trade for every selective query, and
tuning a default to a single benchmark is how benchmarks stop meaning
anything.

**These numbers need re-measuring against a live GCS leg in one session
before being quoted as a ratio.** The bucket was deleted after the earlier
run, so the 1.30 s denominator is from a previous session.
