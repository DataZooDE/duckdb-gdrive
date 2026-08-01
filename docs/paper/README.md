# "A Lakehouse on Google Drive" — paper source

A PVLDB experience-track paper on `duckdb-gdrive`: the design, the guarantees
Drive does and does not provide compared with S3/GCS, and an evaluation
centred on DuckLake over `gdrive://`.

The outline and measurement specification this is being written against lives
at `~/.claude/plans/look-at-https-www-vldb-org-pvldb-vol18-p-async-cloud.md`.

## Build

```bash
make          # -> paper.pdf, and prints the page count
make clean
```

No system LaTeX packages beyond a base `texlive` install are required. The
`acmart` class is vendored under `cls/` rather than assumed present, because a
paper that only builds on the author's machine is a paper nobody else can
check.

**The page count printed by `make` is not yet trustworthy.** `acmart` wants
`libertine`, `newtxmath` and `inconsolata`; none is installed here, so the
build falls back to Computer Modern at different metrics. Install
`texlive-fontsextra` and `texlive-fontsrecommended` before treating the 8–10
page budget as measured. `cls/pifont.sty` is a deliberate no-op stub — see the
comment at the top of that file for why.

## Status

All seven sections are drafted; the document is 10 pages. What remains is
**data, not prose**. Every unmeasured cell renders in red as `[? …]`, so the
gaps are visible in the PDF rather than only in the source.

| Section | Prose | Data |
|---|---|---|
| Abstract, 1 Introduction | drafted | complete |
| 2 Background | drafted | complete |
| 3 Design and implementation | drafted | complete |
| 4 Guarantees and verification | drafted | 2 matrix rows need E-11/E-12 |
| 5 Evaluation | drafted | see below |
| 6 When you should not use this | drafted | complete |
| 7 Related work and summary | drafted | complete |

Outstanding measurements, in the order they should be run:

1. ~~**Figure 1**, per-request latency decomposition~~ — **done and measured**
   (2026-08-01). `make latency_breakdown` re-runs it against live Drive and
   regenerates `data_breakdown.json` and `fig_breakdown.tex`. Result: server
   time-to-first-byte is essentially the entire cost at every request shape,
   which rules out client-side optimisation as a lever and corrected an earlier
   claim that connection reuse was recovering ~224 ms of handshake (it is
   ~35 ms).
2. **Figure 2** — the flame-style wall-clock timeline for a scan, a DuckLake
   read and a DuckLake write. Still a placeholder. Needs richer spans from
   `GDRIVE_TRACE_RANGES` (start timestamp, thread id, call kind, phase splits)
   plus `trace_timeline.py`, and unlike Figure 1 this means touching the C++
   client, not just the Python harness.
3. **Table 4/5** — DuckLake end to end and its local cache, across S3/GCS/Drive.
   The centrepiece, and the largest new harness.
4. Table 3's `s3://` and download-then-query legs; Table 6 write path;
   Figure 3 thread scaling.
5. E-11 and E-12, the two guarantee rows currently marked "unspecified".

## Rules for this manuscript

1. **No number appears here that a harness cannot reproduce.** Every figure in
   a drafted section traces to `docs/benchmark.md`, `docs/benchmark.json`, or a
   named test. Anything not yet measured is written `\todonum{...}` and renders
   in red, so it cannot be mistaken for a result.
2. **Cross-session ratios are not allowed.** The GCS denominator swings 67%
   between sessions — enough to move the headline verdict by more than a full
   multiple on its own. Legs that are compared must be measured in one session.
3. **Every guarantee claim in §4 cites an experiment id** (`\expref{n}`) that
   corresponds to a test which actually runs in `make test_live` or `make e2e`.
   A claim whose test is skipped is not a claim.
4. **Negative results stay in.** The read-ahead buffer, the 32 MiB block size
   that was noise, and the never-measured ~150 ms round-trip figure are each
   the natural first thing to believe, and each was wrong.
