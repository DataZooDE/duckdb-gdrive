# REQ-NF-01 — cold Parquet scan

**Gate:** a cold columnar scan over `gdrive://` completes within **3×** the
same file on Google Cloud Storage.

**Status: EVALUATED, and the gate FAILS at 4.8x–5.7x.** REQ-NF-01 is not
met. The cause is measured, not guessed — 55 Drive round trips for one query
— and the fix is fewer of them, not a softer gate. See *Diagnosis*.

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

At Drive's ~150 ms per round trip that is roughly 8 s of pure latency, which
is essentially the whole measurement. GCS moves the same bytes in ~1.2 s.

Two things stand out, and neither is "Drive is slow":

1. **18 metadata calls for one file.** A single scan should need one. This
   looks like a per-open metadata refresh multiplied by however many times
   DuckDB opens the handle for a parallel scan. Almost certainly avoidable.
2. **35 data requests, uncoalesced.** Adjacent or near-adjacent column
   chunks are fetched as separate ranged GETs. Coalescing them trades a
   little wasted bandwidth for a large latency saving — the right trade when
   a round trip costs 150 ms.

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
relaxed gate. Drive's per-request latency (~150 ms) dominates, so request
count is the lever that matters.
