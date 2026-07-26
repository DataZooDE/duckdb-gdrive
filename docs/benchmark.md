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

Measured 2026-07-26. Linux 6.18, **DuckDB v1.5.5**, extension statically
linked. Fixture: `/fixtures/wide.parquet` (87.0 MB), file id
`17AVQ4WlKaPE3hFxpuoljHLpe3nXI0qsQ`.

| Leg | min | median | vs local |
|---|---|---|---|
| `local` | 0.13 s | 0.13 s | 1.0× |
| `gs://` | *not measured* | — | — |
| `gdrive://` | 5.90 s | 5.97 s | 45× |

**gdrive/GCS ratio: not computable yet.**

### Reading these numbers

The local leg is the floor: it isolates decode cost from transfer cost. At
0.13 s for 87 MB, decode is effectively free here, so essentially all of the
`gdrive` leg's 5.90 s is network — transfer plus per-request latency. That
matters for the gate, because GCS pays transfer cost too. The honest
comparison is Drive's round-trip overhead against GCS's, not against zero, and
the local number is what lets those be separated once the `gs://` leg exists.

**Do not read the `gdrive` figure as stable.** Across runs of this identical
benchmark on the same machine and file we have seen:

| Run | samples |
|---|---|
| earlier | 4.59 s, 4.43 s, **15.43 s** |
| earlier | 4.72 s, 4.57 s, 4.65 s |
| reported above | 5.90 s, 6.03 s, 5.97 s |

The spread between runs (4.43 s to 5.90 s minimum, one outlier at 15.43 s) is
larger than any code change we have made, and nothing on our side differed.
Drive's per-request latency simply varies. This is why the harness reports a
minimum over repeats, why the local leg exists as a control, and why the gate
must be a ratio measured in the same session as its denominator rather than
against a number written down on a different day. A single figure here would
be a marketing number, not a measurement.

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
