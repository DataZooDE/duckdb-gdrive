# REQ-NF-01 — cold Parquet scan

**Gate:** a cold columnar scan over `gdrive://` completes within **3×** the
same file on Google Cloud Storage.

**Status: EVALUATED 2026-07-30. The default configuration sits ON the gate,
at 3.05x; the documented tuned configuration passes at 2.02x.**

Both legs in one session, byte-identical file (md5 verified against the Drive
copy), 9 repeats per leg:

| Leg | min | median | vs `gs` (min) |
|---|---|---|---|
| local | 0.12 s | 0.13 s | — |
| `gs://` (europe-west3) | 1.34 s | 1.65 s | 1.00x |
| **`gdrive://`, default** | **4.09 s** | 4.67 s | **3.05x** — misses by 0.05x |
| `gdrive://`, 128 MiB blocks | 2.70 s | 2.93 s | **2.02x** — passes |

On medians the default is 2.83x, which would pass. Calling this a clean fail
or a clean pass would both be overstating what was measured: it is *at* the
gate.

**The variance is in the DENOMINATOR, and it is large enough to decide the
verdict on its own.** Four full runs were done before settling on 9 repeats:

| Run | `gs` min | `gdrive` min | ratio | verdict |
|---|---|---|---|---|
| 1 (3 repeats) | 1.03 s | 4.25 s | 4.13x | FAIL |
| 2 (3 repeats) | 1.40 s | 4.54 s | 3.24x | FAIL |
| 3 (3 repeats) | 1.72 s | 4.14 s | 2.41x | **PASS** |
| 4 (9 repeats) | 1.34 s | 4.09 s | 3.05x | FAIL |

The `gdrive` leg is stable to ~9% across all four. The `gs` leg swings 1.03 s
to 1.72 s — 67% — and run 3 "passed" purely because GCS happened to be slow,
not because anything improved. A three-repeat gate on this workload reports
noise. `GDRIVE_BENCH_REPEATS` now exists for that reason, and any future
verdict quoted from a 3-repeat run should be distrusted, including the 4.8x
that stood in this document for three days.

**What this means for shipping.** The extension is roughly 3x object storage
out of the box and 2x tuned, against ~30x for a local file. That is the
honest shape and it is what the README and the community-extensions
descriptor now say. The descriptor makes no positive performance claim, so it
is accurate either way.

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

## Results — history

The current result is at the top of this document. This section keeps the
earlier measurements because the *trajectory* is the useful part: what changed,
and what turned out to be measurement error rather than a real effect.

| Date | gdrive | GCS | ratio | verdict | what changed |
|---|---|---|---|---|---|
| 2026-07-27 | 6.21 s | 1.30 s | 4.79x | FAIL | first measurement; 55 round trips per query |
| 2026-07-30 | 4.36 s | — | — | not evaluated | shared block cache landed; no GCS leg that session |
| **2026-07-30** | **4.09 s** | **1.34 s** | **3.05x** | **at the gate** | both legs together, 9 repeats |

Two caveats on the 2026-07-27 row, both learned later and both worth carrying:

* It was a **3-repeat** run. The GCS leg varies by 67% between sessions, which
  is enough on its own to move the verdict by more than a full multiple. Any
  ratio from a 3-repeat run — including that 4.79x, which this document
  presented as settled for three days — should be read as indicative.
* Its GCS leg read the object over `https://storage.googleapis.com/...`
  rather than `gs://`, because DuckDB's `gcs` secret type supported HMAC keys
  only and minting a long-lived credential for a benchmark was not a
  reasonable trade. The current measurement uses a real `gs://` leg with a
  short-lived OAuth2 bearer, which is why `make bench` now needs a gcloud
  login. Both bucket instances were deleted immediately after measurement.

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

The fix is **fewer round trips** — prefetching and coalescing
adjacent byte ranges so a Parquet scan issues fewer, larger requests — not a
relaxed gate. Drive's per-request latency dominates (see *Per-request latency*
below: 0.3–2.1 s depending on the request shape), so request count is the
lever that matters.

## Where the time actually goes

The first diagnosis above — "55 round trips at ~150 ms" — was directionally
right and quantitatively wrong. Measuring properly changed the conclusion.
Where the ~150 ms came from is not recorded anywhere; it appears to have been
assumed. It was published in the README, the BRD and the community-extensions
descriptor for as long as it existed. See *Per-request latency* below for what
it actually is.

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

### How that floor varies with size (measured 2026-07-27)

This is the ranged-read row above, swept across sizes. It is where the "~1.2 s
floor" figure quoted elsewhere in this repo came from, and it applies to
**ranged reads of a large object** — not to Drive requests in general.

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

### What the request pattern actually looks like

Tracing the benchmark query (`GDRIVE_TRACE_RANGES=1`):

* 35 ranged GETs, 54.9 MB of the 87 MB file — so ranged reads do work, and
  the reader is genuinely skipping the columns it does not need.
* Sorted by offset, the gaps between consecutive ranges alternate 1929 KB and
  **0 KB**: half of all requests begin exactly where another ended.
* But those adjacent pairs are on **different file handles** — 35 reads across
  18 handles, and *zero* adjacent pairs share a handle.

That last point killed the obvious fix, and it is worth saying that the
obvious fix was also the *recommended* one: a commissioned research pass on
Drive read performance concluded that coalescing adjacent ranges on a 10–16 MB
threshold "will easily bridge the performance gap". Measured here, it did the
opposite. A per-handle read-ahead buffer was
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

### Block-size sweep, resampled (2026-07-30)

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

Settled at the top of this document: **3.05x at the defaults, 2.02x at 128 MiB
blocks**, measured with both legs in one session.

An earlier estimate here divided a fresh gdrive number by a GCS minimum
remembered from another session. It landed close (~3.4x / ~2.6x) and it was
still the wrong way to produce a number — the real GCS leg varies by 67%
between sessions, so that estimate was luckier than it deserved to be.
