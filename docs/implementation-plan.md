# Implementation plan — `duckdb-gdrive`

> ## RESUME HERE (2026-07-26, 19 commits, pushed to
> ## github.com/DataZooDE/duckdb-gdrive)
>
> **The extension works against real Google Drive.** Live read 34/34, live
> write 23/23, unit 162 cases / 860 assertions. Waves 0–3 are done.
>
> ```bash
> export VCPKG_ROOT=/home/jr/.local/share/vcpkg
> export VCPKG_TOOLCHAIN_PATH=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
> GEN=ninja make && make test                     # unit + credential-free SQL
> set -a && . .env.gdrive && set +a
> ./scripts/run_live_tests.sh                     # live read + write suites
> ```
>
> **Remaining work, in priority order.** Only 1 of the 8 Definition-of-Done
> criteria in §6 is fully met; these close the rest:
>
> 1. **Codex review #2 findings.** A read-path review was running when the
>    session ended — output at `/tmp/.../tasks/bqs0u8f0c.output` if it
>    survived, else re-run per §3.3. Triage into `docs/reviews/`.
> 2. **CI green (criterion 5).** First-ever run started at commit `536a9bc`;
>    `gh run list --repo DataZooDE/duckdb-gdrive`. Never yet been green —
>    expect build-plumbing failures (vcpkg bootstrap, MSVC C++17 leg), not
>    extension bugs.
> 3. **`make bench` (criterion 2).** `wide.parquet` (~100 MB) is seeded.
>    Needs the gs:// counterpart and a committed number in
>    `docs/benchmark.md`. Gate: within 3× GCS.
> 4. **Quota error (criterion 4).** 403/429 rate-limit is classified from
>    documented shapes but has never been provoked live.
> 5. **`check_no_credentials.sh` PEM branch (criterion 7).** KNOWN BROKEN: a
>    planted PEM in `src/` does NOT trip it. The token-shaped branch IS
>    verified. Fix before release.
> 6. **Wave 6 — `erpl-web` migration (criterion 6).** Untouched. This is the
>    debt D-2 took on: `datazoo-oauth2` exists at
>    `/home/jr/Projects/datazoo/datazoo-oauth2` (35/35 green) but `erpl-web`
>    still holds its own copy, so REQ-A-02 is violated and there are two
>    OAuth implementations. **Gate: erpl-web's existing OAuth2 suites must
>    pass UNCHANGED.**
> 7. **Community-extensions PR (criterion 8).** Descriptor staged at
>    `docs/community-extension-description.yml`; set `ref` to a release SHA
>    whose live suite passed.
>
> **Gotchas that cost time before — do not rediscover:**
> - `DRIVE_SCOPE`, never `SCOPE`. `SCOPE` is a reserved DuckDB clause meaning
>   *which paths may use this secret*; using it for the OAuth scope makes the
>   secret match nothing and every query fail with "no secret configured".
> - `ROOT_FOLDER_ID` for a plain folder; `DRIVE_ID` **only** for a real Shared
>   Drive. Mixing them yields `404 "Shared drive not found"`.
> - Reads run as the service account; **writes need the delegated user** — a
>   service account has no Drive storage quota.
>
> ---
>
> **Progress note (2026-07-26).** Wave 0 is complete; the pure-logic layers of
> Waves 1 and 2 are done and green (`make unit_test`). Codex review #1 has run
> and its findings are triaged in
> `docs/reviews/2026-07-26-codex-review-1-wave0.md`.
>
> **Everything touching Drive is unverified**, because D-1 leaves no fake to
> verify against and the CI Shared Drive (§2.1) does not exist yet. That single
> manual step gates: the resolver, ranged reads, listing/pagination, export,
> the write path, the benchmark, and every live SQL test. It is the critical
> path — not a formality.

**Status:** In progress.
**Date:** 2026-07-26
**Companions:** `brd.md` (why), `hld.md` (what). This document is the *how*.
**Target:** DuckDB v1.5.3 stable + v1.4.4 LTS.

---

## 0. Decisions taken (these override the docs where they differ)

| # | Decision | Consequence |
|---|---|---|
| **D-1** | **Real Google Drive is the only integration target. No fake Drive server, no HTTP replay.** Supersedes HLD §10's "integration against a fake". | The merge gate needs the CI service-account secret. Fork PRs cannot run the live suite; they run build + pure-logic tests only, and are labelled as such. Accepted knowingly. |
| **D-2** | **`datazoo-oauth2` is built first and consumed by `duckdb-gdrive` immediately. The `erpl-web` migration is Wave 6**, not Wave 0. Relaxes HLD §5.4's "non-negotiable" ordering. | Drive work starts in week 1. The risk REQ-A-02 guards against (one consumer + one fork) is real and time-boxed: **Wave 6 is committed work, not optional.** Until it lands, no logic change is made to the extracted code that `erpl-web` would need to absorb — the library's API is additive-only during Waves 1–5. |
| **D-3** | **No FUSE/rclone benchmark, no M1 go/no-go gate.** The build proceeds straight through. | BRD §9's strongest alternative stays formally unanswered. It *will* be raised in community-extension review, so the README must address it in prose (§6 of this plan). The GCS-comparison benchmark still ships — it is required by REQ-NF-01 regardless. |
| **D-4** | **Public `github.com/DataZooDE/duckdb-gdrive`, target name `gdrive`**, scheme `gdrive://`. `datazoo-oauth2` public too. | `INSTALL gdrive; LOAD gdrive;` after community acceptance. |
| **D-5** | **Shared Drives are in v1.** Not a preference — a service account has no personal Drive storage quota and cannot own files in a My Drive, so the CI fixtures *must* live in a Shared Drive. Personal Drive is supported for user-OAuth secrets. | Resolves HLD open question 5. The secret carries an optional `drive_id` root binding. |
| **D-6** | **`RemoveFile` trashes by default**; `SET gdrive_permanent_delete=true` opts into `files.delete`. | Resolves HLD open question 4. Trash is recoverable; a table-format cleanup routine deleting the wrong thing on someone's own Drive is unrecoverable. |
| **D-7** | **Docs export to `text/plain`** by default; `SET gdrive_docs_export_mime='text/markdown'` opts in. Sheets always `text/csv`. | Resolves HLD open question 6. Markdown is not byte-stable across exports, which would make `GetVersionTag`-keyed caching lie. |
| **D-8** | **Owner of `datazoo-oauth2`: Joachim Rosskopf** (REQ-NF-06, HLD open question 2). | Named, as the BRD requires before starting. |

---

## 1. What "no mocks" means precisely

The instruction is honoured with a two-layer split that has **no test doubles anywhere**:

| Layer | Contents | Network? | Runs on |
|---|---|---|---|
| **Pure-logic (Catch2 v3)** `test/cpp/test_*.cpp` | Functions that take values and return values: URI parsing, glob pattern matching, `Range:` header construction, Drive-error-JSON → DuckDB exception mapping, path-cache eviction, JWT assertion *construction*. No `#include "duckdb.hpp"`, no I/O, therefore **nothing to mock**. | No | Every PR, every platform |
| **Live SQL (SQLLogicTest)** `test/sql/*.test` | Everything else. Real extension, real secret, real Google OAuth, real Drive API, real files. This is the source of truth. | Yes | main, nightly, on-demand, and any PR from a branch (not a fork) |

If a behaviour needs a socket, it gets a live test — never a double. The module split that makes this possible is `quack-oauth`'s (`docs/IMPLEMENTATION.md` §2.3): one header, a `_pure.cpp` with no DuckDB linkage, a `_duckdb.cpp` with the glue. The Catch2 binary compiles only the former.

**Corollary:** no behaviour gets two tests. If a live SQL test covers it, there is no Catch2 test for the same thing.

---

## 2. The test harness (built before any Drive code)

This is the long pole and the part most likely to be skimped, so it is Wave 0 and it blocks Wave 2.

### 2.1 Google-side setup — **human task, do this first**

Blocking prerequisite; nothing live runs until it exists. `scripts/setup_ci_drive.sh` automates what it can, but a Workspace admin must be in the loop.

1. GCP project `datazoo-gdrive-ci`; enable `drive.googleapis.com`.
2. Service account `gdrive-ci@datazoo-gdrive-ci.iam.gserviceaccount.com`; JSON key downloaded.
3. **Shared Drive** `duckdb-gdrive-ci` created by a `data-zoo.de` Workspace user (`drives.create` requires a Workspace identity — a bare service account cannot do it). Add the service account as **Content Manager**.
4. Second, separate OAuth *client* (Desktop type) for the interactive `authorization_code` tests.
5. Key JSON → GitHub Actions secret `GDRIVE_CI_SA_KEY` (base64). Shared Drive id → `GDRIVE_CI_DRIVE_ID`.

`.env.gdrive.example` documents every variable; `.env.gdrive` is gitignored and CI-scanned for (§5.3).

### 2.2 Fixture strategy — two tiers

Uploading a 100 MB Parquet on every run is wasteful and slow, so fixtures split by mutability:

- **Permanent, read-only** — `/fixtures/` in the Shared Drive, seeded once by `make seed_fixtures` (idempotent; re-uploads only on checksum drift). Contents: `small.csv`, `nested/a/b/deep.csv`, `wide.parquet` (~100 MB, for the REQ-NF-01 benchmark), `parts/part-{00..09}.parquet` (glob), `dup.csv` **twice in one folder** (R-4 collision), a native Sheet, a native Doc, an empty file, a file with spaces and UTF-8 in its name.
- **Per-run scratch** — every mutating test (write, delete, move, mkdir) creates `/scratch/run-<uuid>/`, works inside it, and deletes it in teardown. This is what makes concurrent CI runs safe against one shared Drive.

`make sweep_orphans` (and a nightly CI job) hard-deletes `/scratch/run-*` folders older than 24 h, because crashed runs leak.

### 2.3 Wiring fixtures into SQLLogicTest

SQL tests need the run's folder ids, which are not known until runtime.

- Every live test opens with `require-env GDRIVE_CI_DRIVE_ID` so it **skips cleanly** when unconfigured rather than failing. Non-negotiable — a developer without credentials must still get a green `make test`.
- First task of W0.2: verify whether DuckDB v1.5.3's sqllogictest interpolates `${ENV_VAR}` in query text. If yes, use it. If not, fall back to `*.test.template` + `sed` materialisation into `build/test/sql/`, which is the pattern already proven in `quack-oauth/scripts/run_integration_keycloak.sh`.

### 2.4 Python harness for what SQL cannot express

`e2e/` (uv + pytest + `google-api-python-client`), mirroring `quack-oauth/e2e/`. It lives outside `test/` deliberately so DuckDB's unittest scanner does not walk the venv.

Its job is the things SQLLogicTest genuinely cannot do: provisioning and teardown of scratch fixtures, asserting **API-call counts** (the R-1 amplification metric, read from the extension's own counter — see W4.1), driving the interactive browser OAuth flow headlessly, and the write-then-verify-in-Drive round trip.

### 2.5 Make targets (final shape)

```
make                    # release build (GEN=ninja always)
make test               # pure-logic Catch2 + live SQL that has credentials; skips the rest
make unit_test          # Catch2 only, no network, no credentials
make test_live          # live SQL only; fails loudly if credentials absent
make seed_fixtures      # idempotent permanent-fixture upload
make sweep_orphans      # delete stale scratch folders
make e2e                # uv sync && pytest
make bench              # REQ-NF-01 numbers: gdrive:// vs gs:// vs local
make smoke_static       # ldd allowlist (copy quack-oauth/scripts/check_static_linkage.sh)
make verify_readme      # run every ```sql block in README.md
```

---

## 3. Execution model — waves, parallelism, TDD

### 3.1 The unit of work is a **slice**

Every slice is one red→green→refactor cycle and one commit:

1. **Red.** Write the failing test first, at the layer §1 assigns it. Confirm it fails *for the right reason* (symbol missing / scheme unregistered / secret type unknown) — not a typo.
2. **Green.** Minimum code to pass. No speculation about the next slice.
3. **Refactor.** Only on a green bar; renaming and extracting only, never new behaviour.

Slices below are written as `S-<wave>.<n>` with the red test named explicitly. A slice is not done until its test is green *and* `make unit_test` is still green.

### 3.2 Parallelism

Work is dispatched to **Sonnet 5 subagents**, one per track, each in its own **git worktree** (`isolation: "worktree"`) so parallel tracks never fight over the working tree. Rules that make this safe:

- **One track owns one file set.** Tracks are cut along file boundaries, listed per wave below. Two tracks never edit the same `.cpp`.
- **Shared headers are frozen before fan-out.** The wave lead (me) writes the interface header — pure declarations, no bodies — commits it, and *then* fans out. Agents implement against a fixed contract; nobody negotiates an API mid-flight.
- **`CMakeLists.txt` source list is appended by the wave lead only**, at merge time. It is the single highest-collision file in a DuckDB extension and must not be in any agent's file set.
- **Every agent must leave `make unit_test` green** and hand back the exact test command it ran. An agent reporting "should work" is treated as a failed slice.
- Agents get `model: sonnet`. Wave-boundary integration, the interface headers, and the codex-review triage stay with the lead (Opus).

### 3.3 Codex reviews

`/codex-delegate` at four points, each with a specific question rather than "review this":

| After | Review question |
|---|---|
| **Wave 0** | Is the harness sound? Specifically: can concurrent CI runs corrupt each other's fixtures; does any path leak credentials into logs/errors; does `make test` genuinely stay green with no credentials? |
| **Wave 2** | Read-path correctness: ranged-read boundary arithmetic (off-by-one on `Range: bytes=a-b`, short reads, EOF, zero-length), path-cache invalidation and staleness, the R-4 collision path, error-mapping completeness against Drive's actual error bodies. |
| **Wave 3** | Write/delete/export: the buffered-upload lifecycle (partial write then exception — is the temp buffer leaked, is a half-file created in Drive?), positional-write rejection, export-vs-`alt=media` fork, trash-vs-delete default. |
| **Wave 5** | Pre-submission: community-extension checklist, the static-linkage allowlist, credential hygiene, README claims vs actual behaviour. |

Codex findings are triaged into slices, not fixed ad hoc. Findings judged wrong are recorded with the reason in `docs/reviews/`.

---

## 4. The waves

### Wave 0 — Scaffold, harness, library skeleton  *(≈1 week)*

Three tracks in parallel. **T0.A must merge before T0.B's SQL tests can run**, so T0.A is dispatched first and T0.B starts against a stub.

**T0.A — repo scaffold.** Files: `CMakeLists.txt`, `Makefile`, `extension_config.cmake`, `vcpkg.json`, `.gitignore`, `LICENSE` (MIT), `.github/workflows/*`, `src/gdrive_extension.cpp`, `src/include/gdrive_extension.hpp`.

- `S-0.1` **Red:** `test/sql/gdrive_load.test` — `require gdrive` + `SELECT gdrive_version()`. **Green:** copy `quack-oauth`'s scaffold verbatim — submodules `duckdb`@v1.5.3 and `extension-ci-tools`@v1.5.3, `DUCKDB_CPP_EXTENSION_ENTRY`, and critically the `extension_config.cmake` C++17-`FORCE` block and the bundled-`fmt` MSVC patch. Those two carry hard-won build knowledge; do not re-derive them.
- `S-0.2` **Red:** `make unit_test` fails (no binary). **Green:** Catch2 target + one trivial pure test.
- `S-0.3` CI matrix: stable v1.5.3 + LTS v1.4.4, `exclude_archs: windows_amd64_mingw;wasm_mvp;wasm_eh;wasm_threads` — same reasoning as `quack-oauth` (OAuth redirect flows are not viable in wasm; a clean "unavailable" beats an unloadable artifact).
- `S-0.4` `make smoke_static` with the ldd allowlist.

**T0.B — live test harness** (§2). Files: `scripts/setup_ci_drive.sh`, `scripts/seed_fixtures.py`, `scripts/sweep_orphans.py`, `e2e/**`, `.env.gdrive.example`, `test/README.md`.

- `S-0.5` **Red:** `make seed_fixtures` fails (no script). **Green:** idempotent seeding against the real Shared Drive; prints the fixture folder id.
- `S-0.6` **Red:** a pytest that creates a scratch folder, uploads a file, reads it back, deletes it — fails, no harness. **Green:** `e2e/conftest.py` fixtures. **This proves the credentials work before a line of C++ touches Drive.**
- `S-0.7` `require-env` skip semantics verified: `make test` green on a machine with zero credentials.
- `S-0.8` `make sweep_orphans` + nightly workflow.

**T0.C — `datazoo-oauth2` library** (separate repo). Behaviour-preserving move out of `erpl-web`.

- `S-0.9` Repo scaffold; CMake `add_subdirectory`-consumable; its own Catch2 target.
- `S-0.10` **Red:** library tests referencing `OAuth2Config`/`OAuth2Tokens`/`OAuth2Utils` — fail to compile. **Green:** move `oauth2_types.{hpp,cpp}` unchanged.
- `S-0.11` Same for `oauth2_flow_v2`, `oauth2_server`, `oauth2_browser`, `oauth2_callback_handler`, `http_client`.
- `S-0.12` **Red:** a test asserting PKCE verifier/challenge against the RFC 7636 published test vector. **Green:** already passes — this is the regression net for Wave 6.
- `S-0.13` **Red:** a test asserting `BuildAuthorizationUrl` emits `access_type=offline&prompt=consent` when `extra_auth_params` is set, and emits **nothing extra** when empty. **Green:** add `map<string,string> extra_auth_params` to `OAuth2Config`. The empty case is the `erpl-web` behaviour-preservation assertion.
- `S-0.14` `OAuth2SecretTokenManager` — generalise `MicrosoftEntraTokenManager`, taking token URL + refresh-request builder as inputs. Get-token → check-expiry → refresh → write back to the DuckDB secret.
- `S-0.15` **Red:** `grep -riE 'microsoft|entra|sap|datasphere|google|drive' src/` returns hits. **Green:** zero hits, enforced as a CI check. This is REQ-A-03 made executable.

> Library tests run with no network and no credentials (REQ-A-04). That is not a contradiction of D-1: these are pure functions over strings.

**→ Codex review #1 (harness).**

---

### Wave 1 — Auth  *(≈1 week, parallel with Wave 2)*

Independent of Wave 2 by construction: Wave 2 consumes a single frozen interface, `string GDriveAuth::GetAccessToken(ClientContext&, const string &secret_name)`. The lead commits that header before either wave fans out, so both tracks proceed against it — Wave 2 initially backed by a token pasted from `.env`.

**T1.A — secret type + interactive flow.** Files: `src/gdrive_secret.cpp`, `src/include/gdrive_secret.hpp`.

- `S-1.1` **Red:** `test/sql/gdrive_secret.test` — `CREATE SECRET … (TYPE gdrive, PROVIDER config, …)` errors "unknown secret type". **Green:** register the type + `config` provider (paste pre-obtained tokens).
- `S-1.2` **Red:** `SELECT * FROM duckdb_secrets()` shows a token substring. **Green:** `RedactCommonKeys`. REQ-NF-03 by construction.
- `S-1.3` **Red:** live — `authorization_code` provider against **real Google**, driven headlessly from `e2e/` (loopback redirect caught by the library's `OAuth2Server`; consent automated once, refresh token cached in the CI secret store). **Green:** endpoints via `custom_auth_url`/`custom_token_url` + `extra_auth_params` from `S-0.13`.
- `S-1.4` **Red:** live — a stored refresh token survives a DuckDB restart and yields a fresh access token after the old one is force-expired. **Green:** `OAuth2SecretTokenManager` wired in.

**T1.B — service-account JWT (RFC 7523).** Files: `src/gdrive_service_account.cpp` + `_pure.cpp` split, `src/include/gdrive_service_account.hpp`.

- `S-1.5` **Red (pure):** given a fixed key + fixed `iat`/`exp`, `BuildAssertion()` produces the exact expected header/claims JSON. **Green:** claim construction. Pure — no network, no mock.
- `S-1.6` **Red (pure):** RS256 signature over a known input verifies against the public key. **Green:** `jwt-cpp` + OpenSSL (both already in the vcpkg manifest pattern).
- `S-1.7` **Red (live):** `PROVIDER service_account, KEY_FILE '…'` then a real Drive `about.get` — 200 with the real account's email. **Green:** post the assertion with `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer`.
- `S-1.8` **Red:** malformed key file / wrong scope / clock skew each produce a distinct actionable message, and **no test asserts a message containing key material**.

Scopes default to the narrowest (`drive.readonly`), widening to `drive.file` only when a write secret is declared (REQ-NF-04).

---

### Wave 2 — Read path  *(≈2 weeks; the core)*

Lead freezes `src/include/gdrive_filesystem.hpp` (the `FileSystem` override set from HLD §3) and `src/include/gdrive_client.hpp` before fan-out. Four tracks.

**T2.A — URI + registration.** Files: `src/gdrive_uri_pure.cpp`, `src/gdrive_filesystem.cpp` (registration only).

- `S-2.1` **Red (pure):** parse `gdrive://Finance/2026/actuals.parquet`, `gdrive://id:<fileId>`, `gdrive://<driveName>/…`, trailing slashes, spaces, UTF-8, `gdrive://` alone. **Green:** the parser.
- `S-2.2` **Red (live):** `SELECT * FROM 'gdrive://…'` says "unknown scheme". **Green:** `RegisterSubSystem` + `CanHandleFile`.

**T2.B — HTTP client + error mapping.** Files: `src/gdrive_client.cpp`, `src/gdrive_errors_pure.cpp`.

- `S-2.3` **Red (pure):** each of Drive's real error bodies (404, 401, 403 `insufficientPermissions`, 403 `userRateLimitExceeded`, 429, 500) maps to the exception type and message HLD §9 specifies. Bodies are **captured from real Drive responses** during W0.2 and committed as literal strings — data, not a mock.
- `S-2.4` **Red (live):** reading a non-existent path yields `IOException` naming the path; reading a file the SA cannot see yields `PermissionException` naming the scope. Both provoked against real Drive with real permissions.
- `S-2.5` 5xx retry with jittered backoff, bounded; then surface.

**T2.C — resolver + cache.** Files: `src/gdrive_resolver.cpp`, `src/gdrive_path_cache_pure.cpp`.

- `S-2.6` **Red (live):** resolve `gdrive://fixtures/nested/a/b/deep.csv` to a fileId. **Green:** segment-by-segment `files.list`.
- `S-2.7` **Red (live):** resolving ten siblings in one folder issues **one** folder lookup, not ten (asserted via the API-call counter). **Green:** per-connection prefix cache.
- `S-2.8` **Red (live):** `gdrive://fixtures/dup.csv` (the deliberate duplicate) raises an error naming **both** file ids and suggesting the `id:` form. **Green:** R-4 handling. Never silently pick one.
- `S-2.9` **Red (live):** `gdrive://id:<fileId>` issues **zero** resolution calls. **Green:** direct form.
- `S-2.10` **Red (live):** a secret with `drive_id` set resolves paths relative to that Shared Drive root; `supportsAllDrives`/`includeItemsFromAllDrives` set on every query.
- `S-2.11` **Red (pure):** cache eviction and negative-result handling.

**T2.D — handles, reads, listing.** Files: `src/gdrive_file_handle.cpp`, `src/gdrive_glob_pure.cpp`.

- `S-2.12` **Red (live):** `OpenFile` + `GetFileSize` on `small.csv` returns the true byte length. **Green:** one `files.get`, caching `size`/`modifiedTime`/`headRevisionId`/`mimeType` on the handle.
- `S-2.13` **Red (pure):** `Range:` header for `(location, n)` — including `n=0`, last byte, past-EOF.
- `S-2.14` **Red (live):** `SELECT * FROM read_csv('gdrive://fixtures/small.csv')` returns the right rows. **Green:** ranged `alt=media`. *This is the first end-to-end green.*
- `S-2.15` **Red (live):** `SELECT count(*) FROM 'gdrive://fixtures/wide.parquet'` — the BRD success criterion #1.
- `S-2.16` **Red (live):** `GetVersionTag` returns `headRevisionId`; modifying the file in Drive changes it and invalidates cached blocks.
- `S-2.17` **Red (live):** `ListFiles` over a folder with >100 entries returns all of them. **Green:** pagination — the classic silent-truncation bug.
- `S-2.18` **Red (pure):** glob patterns `*.parquet`, `part-*.parquet`, `**`. **Red (live):** `gdrive://fixtures/parts/*.parquet` reads all ten.
- `S-2.19` **Red (live):** `FileExists`/`DirectoryExists` true and false cases.

**→ Codex review #2 (read path).**

---

### Wave 3 — Native export, write, mutation  *(≈1 week)*

Three tracks, disjoint files.

**T3.A — native Google formats (REQ-F-07).** `src/gdrive_export.cpp`.
- `S-3.1` **Red (live):** reading the fixture Sheet fails with Drive's "only files with binary content can be downloaded". **Green:** on `application/vnd.google-apps.*`, `files.export`.
- `S-3.2` **Red (live):** `SELECT * FROM read_csv('gdrive://fixtures/Budget')` (a real Sheet) returns rows — BRD success criterion #3.
- `S-3.3` **Red (live):** `GetFileSize` on a native file returns the *exported* length, not the absent `size`. Export ignores `Range`, so the whole export is fetched once and cached on the handle; ranged reads are served from that buffer.
- `S-3.4` Doc → `text/plain` (D-7), with the setting honoured.

**T3.B — write path (REQ-F-06).** `src/gdrive_upload.cpp`.
- `S-3.5` **Red (live):** `COPY (SELECT 1) TO 'gdrive://scratch/run-x/out.csv'` errors. **Green:** buffer locally, one resumable upload on close.
- `S-3.6` **Red:** positional `Write(h, buf, n, location)` throws `NotImplementedException` with a message stating Drive cannot write at an offset. Loud beats corrupt (R-3).
- `S-3.7` **Red (live):** `COPY … TO … (FORMAT parquet)` then read it back — round trip.
- `S-3.8` **Red (live):** an exception mid-write leaves **no partial file in Drive** and no leaked temp buffer. Codex review #3 targets exactly this.
- `S-3.9` Overwrite semantics: existing name → `files.update` on the resolved id, not a second file with the same name (which would manufacture an R-4 collision).

**T3.C — mutation + directories.** `src/gdrive_mutate.cpp`.
- `S-3.10` **Red (live):** `RemoveFile` puts the file in **trash**, recoverable (D-6).
- `S-3.11` **Red (live):** with `gdrive_permanent_delete=true`, it is gone.
- `S-3.12` **Red (live):** `CreateDirectory`/`RemoveDirectory` via folder-mimeType files; `MoveFile` via `files.update` on parents/name. Non-atomic rename is documented in the README, not papered over.
- `S-3.13` **Red:** `Truncate`/`Trim` throw `NotImplementedException`.

**→ Codex review #3 (write/delete/export).**

---

### Wave 4 — Performance, quota, hygiene  *(≈0.5 week)*

- `S-4.1` API-call counter exposed as `gdrive_stats()` — calls by kind, cache hits/misses. Needed by `S-2.7`/`S-2.9`, so land it early in Wave 2 if those slices need it sooner; it is listed here because its *reporting* surface belongs to this wave.
- `S-4.2` **Red:** `make bench` does not exist. **Green:** cold 100 MB Parquet scan over `gdrive://` vs `gs://` vs local, same machine, same file; the number committed to `docs/benchmark.md`. **Gate: within 3× GCS (REQ-NF-01).** If it misses, the fix is fewer round trips (prefetch/coalesce adjacent ranges), not a relaxed gate.
- `S-4.3` **Red (live):** deliberately exhaust quota against a throwaway project; assert the message says "Drive API quota" with retry-after, and does **not** read as a generic failure (R-2). If exhaustion proves impractical to provoke, the 403/429 bodies from `S-2.3` stand as the pure-layer assertion and this is documented as a known coverage gap — stated, not hidden.
- `S-4.4` Credential hygiene: CI fails on any file matching Google key-file patterns (`*-[0-9a-f]{12}.json`, `client_secret*.json`, `.env.gdrive`); a test asserts no error path or log line contains a token substring. *(Note: `quack-oauth` currently has a real SA key and a `client_secret*.json` committed at its repo root — that is precisely the failure this check exists to prevent, and worth fixing there.)*
- `S-4.5` `make smoke_static` green on all platforms.

---

### Wave 5 — Release  *(≈0.5 week)*

- `S-5.1` README: install, both secret shapes, the `id:` fast path, Shared Drive binding, the export behaviour, and — because D-3 skipped the benchmark — an explicit "when a FUSE mount or desktop sync is the better answer" section. Community reviewers will ask; answering first is stronger than being asked.
- `S-5.2` `make verify_readme` — every ```sql block executes.
- `S-5.3` Full CI matrix green: stable + LTS, Linux/macOS(arm64+x86_64)/Windows (REQ-NF-05).
- `S-5.4` `docs/UPDATING.md` — the per-DuckDB-release rebuild runbook (REQ-NF-06 is a standing cost; write it down).
- `S-5.5` Community-extensions PR: `description.yml` + repo/ref pin, against `../community-extensions`.

**→ Codex review #4 (pre-submission).**

---

### Wave 6 — `erpl-web` migration  *(≈0.5 week — committed, closes REQ-A-02)*

The debt D-2 took on. Behaviour-preserving by construction.

- `S-6.1` Add `datazoo-oauth2` as a submodule pinned **by commit** (R-9); `add_subdirectory`.
- `S-6.2` Delete `erpl-web`'s `src/oauth2_*.{hpp,cpp}`, `http_client`, and the extracted half of `microsoft_entra_secret.cpp`; repoint includes.
- `S-6.3` **Gate: `test/cpp/test_microsoft_entra_auth.cpp` and `test/cpp/test_datasphere_oauth2_consolidated.cpp` pass UNCHANGED.** If a test needs editing, the extraction was not behaviour-preserving — rework the code, not the test.
- `S-6.4` Full `erpl-web` suite + a live Datasphere/Entra smoke before merge.
- `S-6.5` `grep -c oauth2_flow_v2 erpl-web/src` == 0. No fork remains.

---

## 5. Schedule and dependencies

```
week 1   │ T0.A scaffold ──┐
         │ T0.B harness ───┼─► codex#1
         │ T0.C library ───┘
week 2-3 │ T1.A secret ────┐        (Wave 1 ∥ Wave 2, joined by the frozen
         │ T1.B SA-JWT ────┤         GDriveAuth::GetAccessToken interface)
         │ T2.A uri ───────┤
         │ T2.B errors ────┼─► codex#2
         │ T2.C resolver ──┤
         │ T2.D reads ─────┘
week 4   │ T3.A export ────┐
         │ T3.B write ─────┼─► codex#3
         │ T3.C mutate ────┘
week 5   │ Wave 4 perf/hygiene ─► Wave 5 release ─► codex#4 ─► community PR
week 6   │ Wave 6 erpl-web migration
```

Critical path is Wave 2. The single biggest schedule risk is **§2.1 — the human Google/Workspace setup**; it blocks every live test and needs a Workspace admin. Start it today, before any code.

---

## 6. Definition of done

1. `SELECT count(*) FROM 'gdrive://<folder>/<file>.parquet'` correct against real Drive, clean install, README only. *(BRD #1)*
2. Cold 100 MB Parquet scan within 3× the same file over GCS, number published. *(BRD #2, REQ-NF-01)*
3. A native Sheet queryable with no manual export. *(BRD #3)*
4. Credential, permission and quota errors are each distinct and actionable. *(BRD #4)*
5. CI green on all target platforms, stable + LTS. *(BRD #5)*
6. `datazoo-oauth2` standalone with its own passing tests, **and `erpl-web` consuming it with its OAuth2 suites green and no remaining copy**. *(BRD #6 — Wave 6)*
7. No credential in the repo, in any log, or in any error message. *(REQ-NF-03)*
8. Accepted into `duckdb/community-extensions`; `INSTALL gdrive; LOAD gdrive;` works. *(D-4)*

Criterion 6 is the one D-2 defers. It is not dropped.
