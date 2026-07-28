# Codex review #2 — read path and filesystem (2026-07-26)

Model: gpt-5.5 via `codex exec`, read-only sandbox, ~100k tokens.
Scope: `gdrive_filesystem`, `gdrive_file_handle`, `gdrive_path_cache`,
`gdrive_client`, `gdrive_glob`, `gdrive_uri`, `gdrive_export`, `gdrive_auth`.

**Note on the first attempt.** An earlier run of this review produced a
zero-byte output file and was almost recorded as "found nothing". It had hit
the delegate script's 300-second default timeout, and the script also passed
`--full-auto`, removed in codex-cli 0.145 — the CLI warned and fell through
to reading the prompt from stdin, which hung. Both are fixed. The lesson is
the one this repo keeps relearning: *a tool that reports success while doing
nothing is worse than one that fails.*

## Triage

| # | Severity claimed | Verdict | Action |
|---|---|---|---|
| 1 | Critical — retries duplicate non-idempotent mutations | **Confirmed** | **Mitigated**; full fix needs resumable uploads |

Six of seven are fixed. The seventh is mitigated, with the remaining work
(resumable uploads) stated rather than quietly closed.
| 2 | High — process-global token cache keyed by secret name | **Confirmed, demonstrated** | **Fixed** |
| 3 | High — cached file id survives delete/recreate | **Confirmed, demonstrated** | **Fixed** |
| 4 | High — ranged read accepts 200 and copies from byte 0 | **Confirmed, demonstrated** | **Fixed** |
| 5 | Medium — handle cursor mutated by positional reads | **Confirmed** | **Fixed** |
| 6 | Medium — `FileExists` turns ambiguity/auth errors into false | **Confirmed** | **Fixed** |
| 7 | Medium — range arithmetic can overflow | Confirmed, unreachable in practice | **Fixed** (cheap) |

Nothing was rejected as wrong. One claim the review made *in our favour* was
also checked and holds: the path-resolution cache is keyed by
`(secret_name, drive_id, root_folder_id, canonical_path)` everywhere it is
touched. The token cache was the one that was not.

## Fixed

### 4 — ranged reads could return the wrong bytes (the worst one)

`Download()` accepts HTTP 200 as well as 206, deliberately: a server or an
intermediary proxy may lawfully ignore a `Range` header. But `Read()` copied
from `body.data()` regardless. On a 200 the body is the *whole file*, so a
request for bytes at offset N returned bytes 0..n.

Nothing errors. Every value stays plausible. A Parquet scan simply returns
wrong data. This is the most dangerous shape a bug can take, and no test we
had could see it — every assertion was a count or an aggregate, and those
survive the file being entirely the wrong bytes.

Fixed by applying the offset when the response was not partial, and failing
loudly if the body cannot reach the requested offset. The regression test is
the local-vs-remote identity check borrowed from `duckdb-azure` (see
`test/sql/gdrive_integrity.test.template`), which compares full result sets
rather than aggregates.

### 2 — token cache leaked credentials between databases

The token cache is `static` — one map per *process*, shared by every DuckDB
instance — and was keyed by secret name alone.

Demonstrated on the real API, not argued:

```
CREATE SECRET gdrive (... service_account, real key ...);
SELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');  -- 5
DROP SECRET gdrive;
CREATE SECRET gdrive (... config, CLIENT_ID 'bogus', REFRESH_TOKEN 'bogus' ...);
SELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');  -- 5   <-- LEAK
```

The second read used entirely bogus credentials and succeeded, because it
found the first secret's token under the key `"gdrive"`. In a process hosting
two tenants, that is one tenant reading Drive with another's credential.

Now keyed by name *plus a SHA-256 fingerprint* of everything that determines
which token comes back — for `service_account` the key file's path, size and
mtime plus scope and subject; for `config` the client id, secret and refresh
token. Hashed, so no credential material sits in a map key (REQ-NF-03). The
size+mtime stamp also means a key rotated **in place** invalidates the cache,
which a path-only fingerprint would not.

After the fix the same script fails at the second read with
`401 The OAuth client was not found` — the bogus credentials are actually
used.

### 5 — positional reads raced the shared cursor

The positional `Read` overload wrote `handle.position`. That overload is
`pread(2)`: it takes an explicit offset and must not disturb the cursor.
DuckDB's parallel Parquet reader issues positional reads for different row
groups against **one** handle from several threads, so this was both a data
race and semantically wrong. The sequential overload is now the only writer.

### 7 — range arithmetic

`location` is unsigned and the `Range` header is built from `int64_t`; a
location past `INT64_MAX` produced a negative range. Unreachable with real
Drive files, but the guard is two lines and turns a baffling API error into a
clear one.

## Open, with reasoning

### 1 — retries can duplicate a create (mitigated, not fully fixed)

`ExecuteWithRetry` retries on transport failure for **every** method,
including `POST files.create`. If Drive commits a create and the response
times out, the retry creates a second file with the same name in the same
parent.

This is worse here than in most filesystems, because duplicate names in one
folder are a *hard error* by design (R-4): a retried write can poison the
path permanently, and the user sees "ambiguous" on a path they wrote once.

**Mitigated:** transport-level failures are now retried only for idempotent
methods (GET, DELETE). POST and PATCH return instead, with an error that says
plainly the change *may or may not* have been applied and was deliberately not
retried — because "failed" would imply nothing happened and invite the manual
retry that creates the duplicate.

Not *fixed*, because the real answer is Drive's resumable upload protocol,
whose session URI makes a retry genuinely idempotent. That is a piece of work
rather than a patch, and it also buys chunked recovery for large uploads.
Tracked for Wave 4.

The cost of the mitigation is honest: a write that hits a flaky network now
surfaces an error where it previously might have succeeded on the second
attempt. That is the right trade when the alternative is an unaddressable
path — but it is a trade, not a free win.

### 3 — cached ids survived delete-and-recreate — FIXED

`OpenFile` refreshed metadata by cached file id and ignored the failure, so
once another client deleted and recreated a file at the same path, reads kept
using the dead id — for the life of the process.

A 404 on that refresh now invalidates the cached prefix and re-resolves the
path exactly once. Any *other* refresh failure (a transient 5xx, a rate limit)
still falls back to what the resolver found, rather than failing a read the
cache thought would succeed.

Covered by `e2e/tests/test_stale_cache.py`, which needs all three of a warm
cache in a live process, a mutation performed **outside** that process, and a
second read from it — the middle step is why this lives in the e2e layer and
not in SQLLogicTest. Verified by reverting the fix, where it produces exactly
the predicted symptom:

    IO Error: no such file: gdrive://scratch/.../stale.csv
              (File not found: 1l9wRa3uSzlOSUGIudPv4PrwrbMR7G16_.)

### 6 — `FileExists` swallowed ambiguity and auth failures — FIXED

`FileExists`/`DirectoryExists` caught all exceptions and returned false, so a
duplicate-name ambiguity (R-4) and a 403 both reported "does not exist". A
caller using existence as a guard then proceeds as though the path is free —
which, for a write, is how a third duplicate gets created.

This contradicted the care already taken in `TryResolvePath`, which
deliberately distinguishes not-found from everything else precisely so *Glob*
cannot turn an auth failure into "zero matches". The functions simply were not
using it. Now they do: only not-found becomes `false`.

Verified live: an absent path still returns cleanly, while
`gdrive://fixtures/dup/dup.csv` raises naming both file ids.

## Known coverage gap: positional writes

The README says a positional write "raises an error rather than silently
corrupting the file". The guard exists in `GDriveFileSystem::Write(handle,
buffer, nr_bytes, location)` and throws, but **no test reaches it**, because
nothing in SQL does a positional write:

* `COPY ... TO` is sequential by construction.
* `ATTACH 'gdrive://.../db.duckdb'` does not route to this filesystem at all
  — DuckDB normalises the path and fails earlier with
  `Cannot open file "gdrive:/..."`. Which is fine for users (the README
  already says not to put a database file on Drive) but means it cannot be
  used to exercise the guard.

Recorded rather than left implied. Closing it needs a DuckDB-linked C++ test,
which the Catch2 binary cannot host — that binary is pure-only by policy
(`docs/implementation-plan.md` §1) precisely so it needs no mocks.

## Not expressible yet

The reference suites (`duckdb-azure`) assert credential redaction at the
**transport** layer via `duckdb_logs_parsed('HTTP')` — that no `Authorization`
header and no signature query parameter reaches a log unredacted. We assert
redaction only in error text. Our client does not feed DuckDB's HTTP logger,
so the stronger check is not available to us. Recorded here rather than
quietly skipped.


---

# Codex review #3 — round-trip reduction (2026-07-27)

Reviewed the metadata cache and the proposed block cache. Five findings, all
real; three were bugs in code written that day.

| # | Finding | Action |
|---|---|---|
| 1 | Block cache stored a `200` (Range-ignored) body without the offset fix | **Fixed** — and it was a bug I had fixed hours earlier in the exact-read path and reintroduced in new code |
| 2 | Query scoping ran AFTER `ResolvePath`, so the `id:` form could serve metadata from the previous query | **Fixed** — `BeginQuery` now precedes all resolution |
| 3 | No snapshot isolation: blocks keyed by revision, but the download is by file id | **Documented** — Drive cannot pin a download to a revision; the README now says so |
| 4 | Metadata cache had no single-flight, so concurrent first opens could all miss and all fetch | **Fixed** — `GetOrFetchMetadata` publishes a `shared_future` under the lock |
| 5 | `Glob`'s literal probe neither used nor warmed the query metadata cache | **Fixed** — `Glob` now scopes the query like `OpenFile` |

It also confirmed, independently of the measurements, that a shared block
cache beats per-handle read-ahead here, and that the ~19 opens are DuckDB's
parallel reader rather than anything about our `CanSeek`/`OnDiskFile` flags.

**Process note, worth keeping.** This review failed twice before producing
anything: once at a 300-second timeout, once at 3000 seconds with an empty
output file. Both times `codex exec` sat at "Reading additional input from
stdin" — it was waiting on stdin, not working. Redirecting `< /dev/null` fixed
it. Two hours of apparent work produced nothing, silently, which is the same
failure mode this repo keeps finding in its own checks.


---

# DuckLake conformance sweep (2026-07-28)

`make ducklake_conformance` borrows DuckLake's own test suite and repoints its
DATA_PATH at `gdrive://`, running each test twice -- local and Drive -- and
reporting only tests that pass locally and fail on Drive.

**Result: 0 real findings.** Six tests are meaningfully comparable and all six
pass on both. That is a genuine, if modest, result: appends, CTAS, inlined
data flush and multi-path attach all behave the same on Drive as on a local
disk.

It is modest because of what the numbers exclude, and those bounds matter more
than the headline:

| | |
|---|---|
| 509 DuckLake test files | |
| ~471 need `{DUCKLAKE_CONNECTION}` | a Postgres/MySQL catalog we cannot supply |
| 37 fail on LOCAL too | the DATA_PATH rewrite breaks tests that assert on paths; excluded by design |
| **6 comparable** | all pass on both |

## Three apparent findings that were harness bugs

The first run reported three failures with
`IO Error: Failed to create directory "gdrive://...": No such file or
directory`. That looks exactly like a missing `mkdir -p` in our
CreateDirectory, and I wrote one.

**It was wrong.** The error came from `local_file_system.cpp` -- DuckDB's
LOCAL filesystem -- because the generated tests lacked `require gdrive`, so
the scheme was never registered and every `gdrive://` path fell through to the
local FS. Adding `require gdrive` cleared all three.

The `mkdir -p` change was then tested on its own merits and **reverted**: a
DuckLake data path three levels deep, none of it existing, works with and
without it, because the write path in `OpenFile` creates intermediate folders
and `MutateCreateDirectory` is not the code path DuckLake uses. Shipping it
would have been an unproven behaviour change justified by a misread error.

## And the harness itself reported a false green first

Before any of that, it claimed **43 tests passed on both sides** while
creating zero folders on Drive. Cause: it passed unittest an ABSOLUTE path,
which unittest answers with `No test cases matched` and exit 0 -- so
`returncode == 0` read as a pass and nothing ran at all.

That is the seventh instance of this failure mode in this repo, and the first
inside a tool built specifically to find bugs. `run_one` now treats
`No test cases matched`, `All tests were skipped`, and any output without a
recognisable result line as NOT-COMPARED rather than as agreement.
