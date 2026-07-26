# REQ-NF-01 — cold Parquet scan

**Gate:** a cold columnar scan over `gdrive://` completes within **3×** the
same file on Google Cloud Storage.

**Status: the gate is NOT YET EVALUATED.** The `gdrive://` and local legs are
measured and reproducible; the `gs://` denominator is missing because no
bucket is provisioned. See *What is missing* below. Reproduce with `make
bench`; raw numbers land in `docs/benchmark.json`.

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

Measured 2026-07-26. Linux 6.18, DuckDB v1.5.3, extension statically linked.
Fixture: `/fixtures/wide.parquet`, file id `17AVQ4WlKaPE3hFxpuoljHLpe3nXI0qsQ`.

| Leg | min | median | vs local |
|---|---|---|---|
| `local` | 0.14 s | 0.19 s | 1.0× |
| `gs://` | *not measured* | — | — |
| `gdrive://` | 4.57 s | 4.65 s | 33× |

**gdrive/GCS ratio: not computable yet.**

### Reading these numbers

The local leg is the floor: it isolates decode cost from transfer cost. At
0.14 s for 87 MB, decode is effectively free here, so essentially all of the
`gdrive` leg's 4.57 s is network — transfer plus per-request latency. That
matters for the gate, because GCS pays transfer cost too. The honest
comparison is Drive's round-trip overhead against GCS's, not against zero, and
the local number is what lets those be separated once the `gs://` leg exists.

Drive latency is also **variable**: an earlier run of the identical benchmark
produced 4.59 s, 4.43 s and 15.43 s — one sample 3.4× the others, with no
change on our side. This is why the harness reports a minimum and why a single
number should not be read as a guarantee. It is also an argument for reporting
the ratio with a stated method rather than a marketing figure.

## What is missing

The `gs://` leg needs the same Parquet file in a GCS bucket, and
`GDRIVE_BENCH_GCS_URI` pointing at it:

```bash
gcloud storage buckets create gs://<bucket> --location=EU
gcloud storage cp wide.parquet gs://<bucket>/wide.parquet
GDRIVE_BENCH_GCS_URI=gs://<bucket>/wide.parquet make bench
```

`make bench` exits **non-zero** when the leg is absent. A benchmark that
silently omits its own denominator and prints a green line is worse than no
benchmark, so "not evaluated" is a failure state here, not a footnote.

## If the gate is missed

Per plan §S-4.2 the fix is **fewer round trips** — prefetching and coalescing
adjacent byte ranges so a Parquet scan issues fewer, larger requests — not a
relaxed gate. Drive's per-request latency (~150 ms) dominates, so request
count is the lever that matters.
