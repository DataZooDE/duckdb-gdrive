# Metadata round-trip optimisations — TDD plan

Follow-on from the per-open metadata refresh skip. All five items attack the
same thing Figure 2 of `docs/paper` identified as dominant: **serialised
metadata requests**, not transfer.

Every step is red/green:

1. Write the assertion, run it, **watch it fail for the right reason**. A test
   that passes before the change measures nothing. A test that fails for the
   wrong reason (typo, wrong fixture) is worse — it will "pass" once the typo
   is fixed and prove nothing.
2. Implement the smallest change that makes it green.
3. Re-run the full live suite, not just the new test.
4. Record the measured before/after.

Exact equalities, never `<=`. `files_list <= 151` passes whether the fix works
or not, which is the whole point of the assertion.

## Status

| | state |
|---|---|
| O-1 glob caches its listing | **done**, 151 -> 3 listings, 13.51 s -> 9.25 s |
| O-2 mkdir -p skips impossible probes | **done** for `COPY TO`; **no effect on DuckLake** (see below) |
| O-3 cache created folders | **already existed**; now covered by a test |
| O-4 name-search ancestor collapse | **not built — its gate fired** |
| O-5 batch endpoint | **not built — its gate fired** |
| O-6 extend O-2 to `CreateDirectory` | identified, not built (see below) |

**O-2 does not reach DuckLake.** Its CTAS trace is unchanged at 10 `files.list`
/ 5 `files.create`, because DuckLake creates each directory level through a
separate `CreateDirectory` filesystem call rather than through `OpenFile`'s
mkdir walk. The "we just created this, it is empty" fact is local to one
`OpenFile` and does not survive across calls. Extending it means carrying that
knowledge in the cache, which review flagged as unsound until folder creation
is idempotent or non-retrying (now the latter). Revisit as O-6.

### Review findings fixed (Codex, 2026-08-01)

Three PR-blockers, all confirmed against the code:

1. **O-1 cached descendants of an ambiguous folder.** With a recursive `**`,
   two folders sharing a name each contribute uniquely-named children, so the
   exact-path duplicate guard let `dup/only_a.csv` through. A later open hit
   the full-path entry, never walked `dup`, and never raised R-4 -- a loud
   error turned into a silent choice between Drive branches. Ambiguity now
   propagates to descendants.
2. **`fresh_generation` compared against shared state.** `metadata_generation`
   is one value that whichever thread called `BeginQuery` last has
   overwritten, so two concurrent queries make it thrash and query B could
   treat query A's entry as fresh. Freshness is now compared against a
   thread-local generation.
3. **`files.create` was retried on 5xx.** The transport-failure branch has a
   careful idempotency gate; the HTTP-error branch had none, so a 5xx after
   Drive committed produced a duplicate. It matters most for folder creation,
   which -- unlike file creation -- reserves no id and cannot recognise its own
   retry. Pre-existing, made reachable by O-2/O-3.

## Baseline (measured 2026-08-01, current HEAD + uncommitted work)

| Workload | requests | wall |
|---|---|---|
| 150-file CSV read, cold | 151 `files.list`, 150 `files.media`, **0** `files.get` | 13.51 s |
| DuckLake CTAS, cold | 10 `files.list`, 5 `files.create`, 1 `files.get` | 9.17 s |
| DuckLake read, cold | 6 `files.list`, 1 `files.media` | 2.85 s |
| Parquet scan (`id:` form) | 2 `files.list`, 6 `files.media` | 4.14 s |

---

## O-1 — Glob must cache the children it already listed

**The waste.** `GDriveFileSystem::Glob` calls `ListChildren`, collects into a
local `listing`, matches locally, and returns. It never calls `cache.Put()`.
So a 150-file read lists the directory once and then re-resolves each of the
150 leaves individually — 151 listings where 3 would do. Every leaf was in the
first response, with `size` and `headRevisionId`.

**RED** — `test/sql/gdrive_stats.test.template`:

```
statement ok
SELECT * FROM gdrive_reset_stats();

statement ok
SELECT count(*) FROM read_csv('gdrive://fixtures/many/*.csv');

# 2 to resolve the literal prefix fixtures/many, 1 to list its children.
# Nothing else: every leaf came back in that listing.
query I
SELECT value FROM gdrive_stats() WHERE metric = 'files_list'
----
3
```

Expect it to fail reporting **151**. If it fails with any other number, stop
and understand why before touching the implementation.

**GREEN** — in `Glob`'s `list_folder` lambda, `cache.Put()` each child under
`literal_prefix + "/" + rel`.

Two constraints, both already solved elsewhere in the file:
- **Only unique names.** The glob already computes `name_counts` for
  `DisambiguatePath`. Caching one of a duplicate pair silently reintroduces
  R-4 — a later open would pick it with no error.
- The key must match `CanonicalPathOf` exactly (no leading/trailing slash), or
  it is a cache that never hits.

**Also assert** the answer is unchanged (the suite's existing row counts) and
that `files_media` stays 150 — a "fix" that lost files would pass the count
assertion alone.

---

## O-2 — `mkdir -p` must not probe segments that cannot exist

**The waste.** The DuckLake CTAS trace is `list → create → list → create → …`:
five existence checks, four creates. But once one segment is missing, every
deeper segment is missing too — a child cannot live in a parent that does not
exist. Four of the five listings are unnecessary by construction.

**RED** — a new write case in `test/sql/gdrive_write.test.template`: `COPY` to
`gdrive://scratch/${SCRATCH}/o2/a/b/c/out.parquet` and assert
`files_list` for the write. One listing (for `o2`, the first missing segment),
then none.

**GREEN** — thread a `parent_known_new` bool through the mkdir walk in
`OpenFile`'s write branch; skip `ListByName` once it is set.

**Careful:** the flag must not leak into the *overwrite* check for the leaf
file. If the parent was just created, the leaf cannot exist either — so that
check is also skippable — but only when the parent is genuinely new. Getting
this wrong manufactures an R-4 duplicate.

---

## O-3 — Cache folders we just created

**The waste.** `CreateFolder` returns an id we already know maps to a path, and
nothing is `Put`. A second write into the same directory re-walks the chain.

**RED** — two `COPY`s into the same new deep directory in one session; the
second must cost **0** `files.list` and **0** `files.create`.

**GREEN** — `cache.Put()` after each successful `CreateFolder`. Natural
companion to O-2 and touches the same function.

---

## O-4 — Collapse the cold ancestor walk with a name search

**The waste.** A cold DuckLake read spends 1.66 s on six *serial* listings.
Serial because segment *i+1* needs segment *i*'s id.

**The idea.** One `files.list` with `q = "name='<leaf>' and trashed=false"` and
`fields=files(id,name,parents,mimeType,size,headRevisionId)`, then verify the
returned `parents` chain against the requested path. DuckLake's UUID filenames
are effectively unique, so this is one round trip instead of *depth*.

**This is the risky one and it goes last of the resolution work.** It changes
how paths resolve, and the failure mode is returning the *wrong file*.

Non-negotiable RED tests before any implementation:
- Two files with the **same leaf name in different folders** must each resolve
  to the correct one, not to whichever Drive listed first.
- A leaf whose name matches something outside the requested parent must not be
  returned.
- A genuine R-4 duplicate (same name, same folder) must still raise the
  ambiguity error naming both ids.
- The existing `gdrive_read` suite must pass unchanged.

**GREEN** — search first; on zero, multiple-with-unverifiable-ancestry, or any
uncertainty, **fall back to the segment walk**. The fallback is not an
optimisation detail, it is the correctness guarantee.

Gate: if the ancestry verification needs more than one extra request in the
common case, this is not worth it — abandon and keep the walk.

### OUTCOME: abandoned, gate fired (2026-08-01)

The search returns candidates with their `parents`, and accepting one requires
verifying that chain against the requested path. O-4 only targets a **cold**
process — warm ones already resolve from cache — and cold means no ancestor id
is known, so verification means walking *upward* by `files.get` per level:
`depth − 1` requests against the `depth − 1` listings it replaces. Same order,
no win, plus a fallback path whose failure mode is returning the wrong file.

Accepting a match on a *partial* check (say, only the immediate parent's name)
would be faster and is exactly the "returns the wrong file" outcome this
section forbids. Not built.

One thing it did leave behind: the guard that stops the glob caching leaves
under an ambiguous folder. That is latent today because `TryResolvePath` walks
segment by segment and throws at the ambiguous segment first — but it is
precisely the trap a full-path fast path would spring, so it stays.

---

## O-5 — Drive batch endpoint

Up to 100 sub-requests per HTTP call, `multipart/mixed` encoded.

**Explicitly conditional.** O-1 removes the per-file resolution that is the
main thing worth batching. So:

1. After O-1…O-4 land, re-measure all four baselines.
2. If no workload still shows a double-digit count of same-kind metadata
   requests, **do not implement this** — record that O-1 subsumed it and stop.
3. If one does, batch that specific call kind only.

Batching does not reduce Google's server work and still counts N against
quota; it saves round trips only. It is the highest-complexity item here
(multipart encoding, sub-response parsing, per-sub-request error handling) and
must not be built speculatively.

### OUTCOME: abandoned, gate fired (2026-08-01)

Re-measured after O-1…O-3:

| Workload | metadata requests |
|---|---|
| 150-file read | 3 `files.list` |
| Parquet scan | 2 `files.list` |
| DuckLake CTAS | 10 `files.list`, 5 `files.create` |

The read paths are in single digits — O-1 subsumed the case worth batching.
The write path still shows ten listings, which meets the letter of the gate,
but batching cannot help them for a structural reason: they are
`list → create → list → create`, each needing the id the previous call
returned. **A batch request bundles independent sub-requests; these are
strictly dependent.** No amount of encoding work makes a serial chain
parallel. Not built.

---

## O-6 — extend "just created, therefore empty" to `CreateDirectory`

Where the write-path listings actually go, and the evidence-backed successor to
O-5: **remove** those ten requests rather than bundle them.

DuckLake creates each directory level through its own `CreateDirectory`
filesystem call, so O-2's `parent_is_new` — a local in one `OpenFile` — never
sees them. Making it work across calls means recording "this folder was created
by us during this query, and is empty" somewhere both calls can see.

**Precondition, from review:** this is only sound now that folder creation is
no longer retried on a 5xx. Before that fix a retry could produce a duplicate
folder, and a cached "exists and is empty" entry would hide the resulting R-4
ambiguity permanently.

**Constraint, from review:** cached folder *existence* must never imply
*emptiness* for another thread. Scope the emptiness fact to the query
generation, exactly as `fresh_generation` does, and never let it outlive the
query that established it.

Expected: DuckLake CTAS 10 `files.list` → 1.

---

## Verification after each step

```bash
GEN=ninja make                       # must build clean
./build/release/test/gdrive_unit_tests
./scripts/run_live_tests.sh          # all six suites, not just the new one
cd e2e && uv run pytest -q           # 22 tests
python3 scripts/verify_readme.py
```

Then re-measure the four baselines above and update this table. A change that
improves request counts but not wall-clock is still worth having (Drive
enforces per-user quotas), but say so rather than implying a speed-up.

## Final step

Update `docs/paper/paper.tex` §3.1 and §5.2 with the combined result, and
re-capture the Figure 2 traces (`make trace_figure`) so the timeline matches
the shipped code.
