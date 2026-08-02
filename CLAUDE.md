# duckdb-gdrive — development guide

DuckDB extension registering a `gdrive://` filesystem over Google Drive, so
`read_parquet`, `read_csv`, `COPY`, `glob` and `ATTACH` can address a Drive
file directly with no download step.

See `docs/brd.md` (why) and `docs/hld.md` (what). The decisions below
**override both** where they differ — they were taken during implementation
and the design docs were written before.

## Decisions (D-1 … D-8)

These are referenced by ID from source comments. Changing one means changing
the code that cites it.

| # | Decision | Consequence |
|---|---|---|
| **D-1** | **Real Google Drive is the only integration target. No fake Drive server, no HTTP replay, no test doubles.** Supersedes HLD §10's "integration against a fake". | The merge gate needs the CI service-account secret. Fork PRs cannot run the live suite; they run build + pure-logic tests only. Accepted knowingly. |
| **D-2** | `datazoo-oauth2` is built first and consumed here immediately; the `erpl-web` migration is deferred. | Time-boxed debt against REQ-A-02. Until it lands, no logic change is made to the extracted code that `erpl-web` would need to absorb. |
| **D-3** | No FUSE/rclone benchmark, no go/no-go gate. | BRD §9's strongest alternative stays formally unanswered, so the README answers it in prose ("When you should NOT use this"). The GCS comparison still ships — REQ-NF-01 requires it. |
| **D-4** | Public `github.com/DataZooDE/duckdb-gdrive`, target name `gdrive`, scheme `gdrive://`. | `INSTALL gdrive; LOAD gdrive;` after community acceptance. |
| **D-5** | **Shared Drives are in v1.** Not a preference: a service account has no personal Drive storage quota and cannot own files in a My Drive, so CI fixtures *must* live in a Shared Drive. | The secret carries an optional `drive_id` root binding. Personal Drive works for user-OAuth secrets. |
| **D-6** | **`RemoveFile` trashes by default**; `SET gdrive_permanent_delete=true` opts into `files.delete`. | Trash is recoverable; a table-format cleanup routine deleting the wrong thing on someone's own Drive is not. |
| **D-7** | **Docs export to `text/plain`** by default (`gdrive_docs_export_mime` to change); Sheets always `text/csv`. | Markdown is not byte-stable across exports, which would make `GetVersionTag`-keyed caching lie. |
| **D-8** | Owner of `datazoo-oauth2`: Joachim Rosskopf (REQ-NF-06). | — |
| **D-9** | **No embedded OAuth client.** `PROVIDER authorization_code` will require the user's own `CLIENT_ID`/`CLIENT_SECRET`. | We deliberately do not match `gsheets`' `CREATE SECRET (TYPE gsheet);` one-liner, which is possible only because Evidence embeds a client id and hosts a redirect page. In exchange: no Google verification track, no CASA assessment for restricted Drive scopes, no 100-test-user cap, and nobody's consent screen says "DataZoo". The comfort gap is closed by D-10 instead. |
| **D-10** | **`credential_chain` is the default provider**, resolving Application Default Credentials ourselves rather than via `google-cloud-cpp`. | `CREATE SECRET (TYPE gdrive);` works with no arguments after `gcloud auth application-default login`. See the pitfall below for why the SDK was rejected. The default provider changed from `config`, which could never succeed argument-less, so no working statement changed meaning. |
| **D-11** | `datazoo-oauth2` is public at `github.com/DataZooDE/datazoo-oauth2`, consumed as a commit-pinned submodule. | Forced by community-extensions building this repo from source: a private submodule cannot be cloned by their builder. Reaffirms HLD §5.4's rejection of vendoring. |
| **D-12** | The `erpl-web` migration gate runs that repo's **live** OAuth suites, not just its offline Catch2 binaries. | Needs real SAP Datasphere / Entra / Business Central credentials. Consistent with D-1: a suite that only proves the code compiles is not a gate. |
| **D-13** | **CalVer, `vYYYY.MM.DD`**, matching the other DataZoo extensions. | The community descriptor carries **no `version:` field** — `2026.08.01` is not valid semver (leading zeros), so the tagged `ref` is the version. `src/gdrive_version.cpp` is a hardcoded string and `make check_stamp` fails if it does not match the tag; nothing compared them before v2026.08.01. |
| **D-14** | **`gdrive_immutable_prefixes` is opt-in and empty by default.** | It skips the per-open metadata refresh for paths the user declares are never modified in place. The extension cannot verify the claim: Drive keeps a file id across an overwrite, returns no `ETag`, and *silently ignores* `If-Match` (verified 2026-08-01 — a bogus precondition returns `206` and the data, not `412`). So it is a user assertion, and the promise is stronger than "not overwritten": not modified, not replaced, and not deleted-and-recreated while the process runs. |

Target name: **`gdrive`** — `INSTALL gdrive; LOAD gdrive;`, and `TARGET_NAME`
in `CMakeLists.txt`.

### A note on `S-x.y` in comments

Source comments carry work-item IDs like `S-2.11` or `S-3.8`. They came from a
build plan that has since been removed — it was ~60% schedule and wave
sequencing that stopped being true the moment the thing shipped. The IDs are
left in place because they appear throughout the commit history, so
`git log --grep=S-2.11` still finds the reasoning and the red/green cycle for
any of them. They are historical labels, not pointers to a live document.

## Knowledge updates

This file is a living document. When a session uncovers something a future
session would want to know — a persistent error and its fix, a non-obvious
DuckDB C++ API behaviour, a vcpkg or CMake quirk, a Drive API surprise, a
working pattern for something fiddly — capture it here. A paragraph under the
right section is enough. The bar: *would past-me have saved an hour if this
had been written down?*

## Build

Always use ninja; `make` without it takes 2–3× longer.

```bash
export VCPKG_ROOT=/home/jr/.local/share/vcpkg
export VCPKG_TOOLCHAIN_PATH=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
GEN=ninja make                # release, the usual dev loop
GEN=ninja make debug          # debug (ASAN, lldb)
```

`make check_pin` (also a CI job) fails if the `duckdb` submodule has drifted
off the shipped tag. This is not bookkeeping: the submodule once sat ~9,600
commits past its tag on DuckDB main, every local build was green, and every
release build was red -- `BaseSecret::GetName()` returns `const string &` on
all released 1.4/1.5 versions and `const Identifier &` on main. Local builds
target **latest stable**; CI additionally builds the **1.4 LTS** line, so a
local green is necessary but not sufficient.

Artifacts:

- `build/release/duckdb` — shell with the extension statically linked
- `build/release/test/unittest` — DuckDB's test runner (runs the SQL tests)
- `build/release/test/gdrive_unit_tests` — Catch2 pure-logic binary
- `build/release/extension/gdrive/gdrive.duckdb_extension` — the shipping artifact

First build is slow (DuckDB + vcpkg openssl/jwt-cpp). Incremental builds are
seconds. `make clean` is rarely needed — only after editing `vcpkg.json` or
bumping the DuckDB submodule.

## Tests — two layers, no mocks anywhere

Two layers, **no test doubles anywhere** (D-1). If a behaviour needs a socket
it gets a live test, never a double; if it does not, it gets a Catch2 test.
**No behaviour gets both** — a live SQL test covering something means there is
no Catch2 test for the same thing.

| Layer | What | Network | Command |
|---|---|---|---|
| **Catch2** `test/cpp/test_*.cpp` | Pure functions only: URI parsing, glob matching, error classification, JWT claim construction. No `duckdb.hpp`, no I/O — so there is **nothing to mock**. | no | `make unit_test` |
| **Live SQL** `test/sql/*.test` | Everything else, against **real Google Drive**. | yes | `make test` / `make test_live` |

There is **no fake Drive server and no HTTP replay layer** (decision D-1). If
a behaviour needs a socket it gets a live test. If it does not, it gets a
Catch2 test. No behaviour gets both.

`make test` lets live tests **skip** when credentials are absent, so a
developer without a Google account still gets a green run. `make test_live`
**fails** instead — CI uses that, because a silent skip is a false green.

Run one thing:

```bash
./build/release/test/unittest test/sql/gdrive_load.test
./build/release/test/gdrive_unit_tests "[uri]"        # by tag
```

## Live-test credentials

One-time setup: `./scripts/setup_ci_drive.sh`, then `make seed_fixtures`.

**A Shared Drive is required, not preferred.** A service account has no
personal Drive storage quota; uploading anything outside a Shared Drive
returns `403 storageQuotaExceeded` with the message *"Service Accounts do not
have storage quota. Leverage shared drives…"*. Folder *creation* succeeds
(folders consume no quota), so the failure looks like a permissions problem
right up until you try to write a byte. Verified against the real API
2026-07-26.

Creating the Shared Drive itself needs a Google **Workspace user** —
`drives.create` refuses a bare service account. That step is manual and the
setup script walks you through it.

### Two identities, and why

Reads run as the **service account**; writes and fixture seeding run as a
**delegated user**. That is not a style choice — a service account has no
Drive storage quota and gets `403 storageQuotaExceeded` on *any* upload
outside a Shared Drive, so it literally cannot create the fixtures it then
reads.

`make oauth_consent` does the one-time browser consent and stores the refresh
token in `.env.gdrive`.

**CI** cannot do browser consent, so the token is minted once locally and
stored as repository secrets:

| Secret | Used for |
|---|---|
| `GDRIVE_CI_SA_KEY_B64` | service-account key, base64 — reads |
| `GDRIVE_CI_DRIVE_ID` | the fixture root (folder id, or a Shared Drive id) |
| `GDRIVE_OAUTH_CLIENT_ID` / `GDRIVE_OAUTH_CLIENT_SECRET` | the OAuth client |
| `GDRIVE_USER_REFRESH_TOKEN` | delegated user — writes and seeding |

A Google refresh token does not expire on a timer, but it *is* revoked if the
user revokes access, if the OAuth client is deleted, or after ~6 months
unused on a project still in "Testing" publishing status. If the live job
starts failing auth, re-run `make oauth_consent` and update the secret — that
is the expected maintenance, not a bug.

Fixtures are two-tier: permanent read-only `/fixtures` seeded
once, and per-test `/scratch/run-<uuid>` deleted on teardown so concurrent CI
runs cannot collide. `make sweep_orphans` cleans up after crashed runs.

**The nightly sweep had been dead since 2026-07-29 and nobody noticed**, for
three independent reasons worth remembering. (1) The workflow job checks out
WITHOUT submodules, and the Makefile includes
`extension-ci-tools/makefiles/duckdb_extension.Makefile` at the top, so `make`
died on the include before reaching any target -- a job that fails in 3
seconds looks like an infrastructure blip. (2) `sweep()` let a single failed
delete propagate, so even run by hand it removed one folder of 61 and stopped.
(3) It authenticated as the SERVICE ACCOUNT, but scratch folders are created by
writes, and writes run as the DELEGATED USER -- the user owns them, and a
service account cannot permanently delete a file it does not own in a Shared
Drive. Measured 2026-08-02: 0 of 58 deletable as the SA, 58 of 58 as the user.
The sweep now runs the module directly (no submodules needed), collects
per-folder failures and exits non-zero, and authenticates as the user.

**Do not run `make seed_fixtures` while live tests are running.** Seeding
deletes a fixture before re-uploading it, so a concurrent test can observe
the gap and fail for a reason that has nothing to do with the code. The
robust fix is staging plus a pointer swap; it was judged not worth the work
for a case that only arises if someone seeds during CI, so the nightly sweep
and seed jobs are simply scheduled not to overlap. Revisit if it ever bites.

## Repository layout

```
src/
  gdrive_extension.cpp        entry point: DUCKDB_CPP_EXTENSION_ENTRY -> LoadInternal
  gdrive_uri.cpp              PURE: gdrive:// parsing
  gdrive_errors.cpp           PURE: Drive error -> DuckDB exception classification
  gdrive_glob.cpp             PURE: local glob (Drive's API cannot glob)
  gdrive_service_account_pure.cpp  PURE: RFC 7523 assertion construction
  include/                    one header per module; the pure/DuckDB split
                              shares a header
test/cpp/                     Catch2, pure logic only
test/cpp/testdata/            REAL captured Drive error bodies — data, not mocks
test/sql/                     SQLLogicTest; *.test.template gets fixture ids
                              substituted at run time into test/sql/live/
e2e/                          uv + pytest harness: fixture provisioning,
                              API-call-count assertions, write round trips
duckdb/                       submodule, v1.5.5 (latest stable)
extension-ci-tools/           submodule, v1.5-variegata (rolling 1.5 branch)
```

## Pure vs DuckDB source split

A module with both pure logic and DuckDB integration is split into two `.cpp`
files sharing one header. The Catch2 binary compiles only the pure one; the
extension compiles both. `CMakeLists.txt` **GLOBs** the pure sources rather
than listing them — it is the highest-collision file in the repo and several
tracks add sources concurrently.

Pure code must not `#include "duckdb.hpp"`, must not do I/O, and must not
throw across its API — it returns a result struct with an error string, and
the DuckDB-side caller turns that into the right exception type.

## Working several tracks in parallel

`CMakeLists.txt` GLOBs the pure sources so concurrent tracks never contend on
it. The trade-off, learned the hard way: **one track's red test breaks
everyone's build**, because all pure sources link into one Catch2 binary. A
track that has written its failing test but not yet its implementation leaves
`make unit_test` unlinkable for every other track.

So when verifying one track's work while another is mid-slice, compile that
track standalone instead of waiting:

```bash
CATCH_INC=$(ls -d build/release/vcpkg_installed/*/include | head -1)
CATCH_LIB=$(ls -d build/release/vcpkg_installed/*/lib | head -1)
g++ -std=c++17 -Isrc/include -I"$CATCH_INC" \
    test/cpp/test_uri.cpp src/gdrive_uri.cpp \
    -L"$CATCH_LIB" -lCatch2Main -lCatch2 -o /tmp/t && /tmp/t
```

## Common pitfalls

**The build needs at least one git commit.** DuckDB's
`duckdb_extension_generate_version` runs `git log -1 --format=%h` and fails
configure with a confusing "git has failed to execute" on a repo with no
commits.

**`git submodule add --depth 1 -b v1.5.3` fails on a tag.** `-b` wants a
branch; the checkout step then tries to create a local branch from
`origin/v1.5.3` and dies. Add the submodule, then `git -C <path> checkout
v1.5.3`. Note `extension-ci-tools` uses version *branches*, `duckdb` uses
*tags*.

**C++17 must be forced for the whole build**, not just our targets — see the
long comment in `extension_config.cmake`. Two separate failures (a
weak/strong symbol clash on Linux/GCC, `error C7525` in bundled `fmt` on
MSVC) both trace back to DuckDB caching `CMAKE_CXX_STANDARD=11`.

**`PICOJSON_USE_INT64` must be defined project-wide** (it is, in
`CMakeLists.txt`). picojson stores parsed numbers as `int64_t` only when that
macro is set before its first include in a TU, else as `double`. jwt-cpp's
`traits/kazuho-picojson/defaults.h` defines it; a direct
`#include <picojson/picojson.h>` does not. Mixing both in one binary is an ODR
violation whose symptom — whether `get<int64_t>()` works or throws — depends
on link order. Do not "fix" this per-file.

**Drive API calls must set `supportsAllDrives` and
`includeItemsFromAllDrives`.** Without them the API behaves as if Shared
Drives do not exist and returns "not found" for files that plainly do.

**`google-cloud-cpp` cannot authenticate to Drive with a service account.**
Its `oauth2::MakeAccessTokenGenerator`, given service-account credentials,
returns a **self-signed JWT** (RS256, three segments, ~750 chars) rather than
an OAuth2 access token. Google *Cloud* APIs accept those; Drive — a Workspace
API — answers `401 Invalid Credentials`. `ScopesOption` does not help: it is
documented as configuring `MakeImpersonateServiceAccountCredentials()` only.
Verified against the real API 2026-07-31 with a real key. The user-ADC arm is
fine (a real 256-char `ya29.` token). This is why `credential_chain` resolves
ADC itself (D-10) instead of following `northpolesec/duckdb-gcs` all the way —
that extension only ever talks to GCS, where the self-signed JWT works.
Diagnostic that makes this obvious in seconds: count the dots in the token.
Two means JWT, one means `ya29.`.

**gcloud's default ADC scope does not include Drive.** `gcloud auth
application-default login` requests `cloud-platform`, so every Drive call
returns `403` — and `errors[0].reason` is `insufficientPermissions`, exactly
what a genuine file-sharing denial returns. The only discriminator is
`error.details[].reason == "ACCESS_TOKEN_SCOPE_INSUFFICIENT"`, which is why
`ParseErrorBody` reads `details[]` at all and why `INSUFFICIENT_SCOPE` is a
separate `GDriveErrorKind`. Without that split the message sends people to
audit Drive sharing for a problem entirely inside their token. Fix is
`--scopes=openid,https://www.googleapis.com/auth/drive`.

**Consuming `datazoo-oauth2` has three traps, all hit for real.** The library
was extracted from `erpl-web` and kept `namespace erpl_web`, so its headers
collide with a consumer that still has its own copies of anything. (1) Its
`tracing.hpp` defines `ERPL_TRACE_*` and an `erpl_web::ErplTracer` alias —
define `DATAZOO_OAUTH2_USE_HOST_TRACING` if you have your own. (2) Its
`http_client.hpp` does `using namespace duckdb;` at **global** scope; include
it earlier than the previous order did and unqualified names start resolving
into `duckdb::` — this made `EnumType` ambiguous in an erpl-web test file that
neither header had ever been near. Include the tiny `odata_version.hpp` when
that is all you need. (3) DuckDB *exports* the extension target, so any
library it links must `install(... EXPORT "${DUCKDB_EXPORT_SET}")` or CMake
refuses to generate. All three are fixed upstream now; the pattern is what to
remember.

**`OAuth2Browser` does not check for a display.** It shells out to `xdg-open`
and hopes, so on a headless host the flow blocks until the callback handler
times out and then reports a *timeout* — which reads as "you were too slow"
rather than "this machine has no browser". `CanLaunchBrowser` in
`gdrive_oauth_params.cpp` is checked before the flow starts. Note
`SSH_CONNECTION` is deliberately not a veto: with X11 forwarding `DISPLAY` is
set and the browser really does open, on the user's own machine. Its
`IsPortAvailable*` are also stubs that always return true, on every platform —
pinned by a regression test in the library, so do not "fix" them there.

**Drive permits two files with the same name in one folder.** A path is
therefore not a unique identifier (R-4). Multiple matches are an error naming
both ids — silently picking one makes query results depend on Drive's
internal ordering.

**A drifted `duckdb` submodule makes every local green meaningless.** It once
sat ~9,600 commits past its tag on DuckDB main; every local build passed and
every release build failed, because `BaseSecret::GetName()` returns
`const string &` on all released 1.4/1.5 versions and `const Identifier &` on
main. `make check_pin` now enforces the tag. Local builds cover **stable
only** — CI also builds the 1.4 LTS line, so a local pass is necessary but not
sufficient.

**Drive picks the native type from the SOURCE content type, not the target.**
`files.create` with `mimeType: application/vnd.google-apps.document` and a
`text/csv` body produces a **Sheet**, silently. Upload `text/plain` for a Doc
and `text/csv` for a Sheet (`Drive._NATIVE_SOURCE_MIME` in
`e2e/helpers/drive.py`). Because of this the "Notes" fixture was a spreadsheet
for a long time and every "create only when absent" seeding run skipped it —
seeding now verifies the mimeType instead of treating present as correct.

**A grep pattern starting with `-` is parsed as options.** `grep -qE "$pat"
"$f"` where `$pat` begins `-----BEGIN` prints usage and exits **2**, and
`if grep ...; then` reads that as "no match". Always `grep -qE -e "$pat" --
"$f"`, and treat exit status >= 2 as an error rather than a miss — that is how
the credential scanner silently never looked for private keys.

**Drive transfers need a size-based timeout.** A flat 60 s socket timeout kills
a 100 MB upload mid-body with a bare `ConnectionError` naming neither the file
nor the size. See `Drive._transfer_timeout()`.

**After moving the `duckdb` submodule you MUST `rm -rf build/release`.**
CMake caches the DuckDB version it computed at configure time. Keep a stale
build directory and every later `make` produces an extension stamped for the
OLD DuckDB -- ours was stamped `d8cdaa33fd` instead of `v1.5.5` and refused to
load into any stock DuckDB, while the whole suite stayed green because the
tests use the statically linked shell and never load the shipped artifact.
`make check_stamp` and `make smoke_loadable` now catch this; the warning in
the Build section above was already there and was not enough.

**DuckDB versions ITSELF from `git describe --tags` inside `duckdb/`.**
`actions/checkout` does not fetch submodule tags, so without an explicit
`git -C duckdb fetch --tags` the build falls back to a dummy `v0.0.1` and
stamps the artifact for a DuckDB that does not exist. Verified:

```
with tags:    v1.5.5-0-gd8cdaa33fd
without tags: fatal: No names found, cannot describe anything.
```

The Checks workflow now fetches them. Release artifacts were never affected —
the distribution pipeline checks DuckDB out properly.

**A new .cpp must include `<cstdint>` itself, even if its header does.** The
musl gate caught `gdrive_trace.cpp` in 3 seconds of CI: the TU named `uint64_t`
and glibc supplied it transitively where musl does not, and musl is in the 1.4
LTS matrix. `make check_cstdint` exists for exactly this and is worth running
before pushing a new source file.

**`fopen(path, "we")` is glibc-only.** The `e` (O_CLOEXEC) mode extension is
not standard C and is undefined behaviour on MSVC's CRT. This repo ships to
glibc, musl, macOS and Windows, so plain `"w"` it is.

**The two retry branches were not symmetric.** `ExecuteWithRetry`'s
transport-failure path has a careful idempotency gate — a retried POST
`files.create` manufactures an R-4 duplicate — but the HTTP-error path (5xx,
429) had none until 2026-08-01. A 5xx is exactly as ambiguous as a dropped
connection. It bit hardest on FOLDER creation, which unlike file creation
reserves no id via `files.generateIds` and so cannot recognise its own retry.

**Read-time stale-id recovery is not "strictly better" than validating at
open**, whatever the old comment said. It covers more (a file deleted *after*
the handle opened), but DuckDB is told the file size at open and no read-time
mechanism can retract a number already reported, so a different-length
replacement reads short. It converts a hard failure into a bounded read.

**DuckLake has no local data-file cache of its own** (checked at `d8a1881e`).
`duckdb_settings()` returns retry and inlining knobs and nothing about caching
data files; the relevant control is DuckDB core's
`enable_external_file_cache`, which is IN MEMORY and therefore does nothing for
the cold-process workload that matters on Drive (measured: every delta inside
the noise, on all three backends). `duckdb-diskcache` is a separate extension.
Do not cite a DuckLake cache setting from memory — enumerate it.

**A benchmark needs its cloud logins checked first.** `gcloud` and `aws`
sessions both expired mid-session during the 2026-08-01 measurements, one of
them between creating a bucket and uploading to it. `make bench` fails loudly
on a missing leg, which is right, but the reauth is interactive and cannot be
done from a script.

**Real-world drill, 2026-08-02 — what live use broke that unit tests did not.**
Six defects, every one of them silent or a wrong answer rather than an error.
The pattern worth remembering: *`FileExists` returning false is not proof of
absence.* `GDriveFileSystem::FileExists` deliberately answers false when no
secret is configured, because DuckDB core probes speculatively during
replacement-scan binding (`bind_basetableref.cpp`) and during direct file reads
(`direct_file_reader.cpp`) and would break if it threw. That leniency is right
for core and wrong as a user-facing contract: `file_size` and `remove_file`
built "does not exist" on top of it and reported an unconfigured extension as a
missing file. They now confirm absence with an explicit
`FILE_FLAGS_NULL_IF_NOT_EXISTS` open, which the gdrive `OpenFile` had also been
ignoring outright.

Also: writing under a path whose parent is a FILE used to create a same-named
FOLDER beside it — the extension manufacturing the exact R-4 duplicate it
refuses to create elsewhere, and making the original file unreadable. And
`glob` returned folders for a literal path, though the listing branch filtered
them; local `glob` of a directory returns zero rows, which is the parity to
match.

**Cost assertions belong in their own test file.** The mkdir depth-invariance
check lived in `gdrive_write.test` and started failing the moment an unrelated
fix changed how DuckDB probes a COPY target — the property was fine, the shared
scratch state was not. It is now `gdrive_mkdir_cost.test.template`, which
touches its own subtrees and nothing else. Note also that anything appended to
`gdrive_write.test.template` must go BEFORE the service-account section: that
block drops the delegated-user secret and runs as an identity with no Drive
storage quota, so a write after it cannot succeed.

**Concurrent directory creation duplicates too, not just files.** Three
parallel `COPY` calls into one new subtree produced three copies of the
top-level folder, and every writer reported success. `OpenFile`'s mkdir walk
now adopts a folder another writer created if its second listing sees one,
which narrows the window; it cannot close it, because Drive has no atomic
create-if-absent. A create-then-reconcile (delete the losing duplicates) was
considered and rejected: another writer may already have written into the one
you would delete. Documented as a limitation instead.

**Releasing:** follow `docs/RELEASE.md`. The community-extensions repo builds
from a tagged commit and runs none of our live tests, so whatever is broken
at tag time ships.

## Credential hygiene

`make check_credentials` (also a CI job) fails on tracked files matching
Google key-file patterns and on PEM private-key content in tracked files.
`.gitignore` only protects people who never ran `git add -f`.

Tokens live in memory or DuckDB secrets, are never written to disk by this
extension, and never appear in error text — there are tests asserting the
last part specifically (REQ-NF-03).
