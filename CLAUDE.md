# duckdb-gdrive — development guide

DuckDB extension registering a `gdrive://` filesystem over Google Drive, so
`read_parquet`, `read_csv`, `COPY`, `glob` and `ATTACH` can address a Drive
file directly with no download step.

See `docs/brd.md` (why), `docs/hld.md` (what), `docs/implementation-plan.md`
(how we build and test it — **read section 0 first: it records decisions that
override the other two documents**).

Target name: **`gdrive`** — `INSTALL gdrive; LOAD gdrive;`, and `TARGET_NAME`
in `CMakeLists.txt`.

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

Artifacts:

- `build/release/duckdb` — shell with the extension statically linked
- `build/release/test/unittest` — DuckDB's test runner (runs the SQL tests)
- `build/release/test/gdrive_unit_tests` — Catch2 pure-logic binary
- `build/release/extension/gdrive/gdrive.duckdb_extension` — the shipping artifact

First build is slow (DuckDB + vcpkg openssl/jwt-cpp). Incremental builds are
seconds. `make clean` is rarely needed — only after editing `vcpkg.json` or
bumping the DuckDB submodule.

## Tests — two layers, no mocks anywhere

Policy in `docs/implementation-plan.md` §1. Summary:

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

Fixtures are two-tier (plan §2.2): permanent read-only `/fixtures` seeded
once, and per-test `/scratch/run-<uuid>` deleted on teardown so concurrent CI
runs cannot collide. `make sweep_orphans` cleans up after crashed runs.

## Repository layout

```
src/
  gdrive_extension.cpp        entry point: DUCKDB_CPP_EXTENSION_ENTRY -> LoadInternal
  gdrive_uri.cpp              PURE: gdrive:// parsing
  gdrive_errors.cpp           PURE: Drive error -> DuckDB exception classification
  gdrive_glob.cpp             PURE: local glob (Drive's API cannot glob)
  gdrive_service_account_pure.cpp  PURE: RFC 7523 assertion construction
  include/                    one header per module; the pure/DuckDB split
                              shares a header (plan §1)
test/cpp/                     Catch2, pure logic only
test/cpp/testdata/            REAL captured Drive error bodies — data, not mocks
test/sql/                     SQLLogicTest; *.test.template gets fixture ids
                              substituted at run time into test/sql/live/
e2e/                          uv + pytest harness: fixture provisioning,
                              API-call-count assertions, write round trips
duckdb/                       submodule, v1.5.3
extension-ci-tools/           submodule, v1.5.3
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

**Drive API calls must set `supportsAllDrives` and
`includeItemsFromAllDrives`.** Without them the API behaves as if Shared
Drives do not exist and returns "not found" for files that plainly do.

**Drive permits two files with the same name in one folder.** A path is
therefore not a unique identifier (R-4). Multiple matches are an error naming
both ids — silently picking one makes query results depend on Drive's
internal ordering.

## Credential hygiene

`make check_credentials` (also a CI job) fails on tracked files matching
Google key-file patterns and on PEM private-key content in tracked files.
`.gitignore` only protects people who never ran `git add -f`.

Tokens live in memory or DuckDB secrets, are never written to disk by this
extension, and never appear in error text — there are tests asserting the
last part specifically (REQ-NF-03).
