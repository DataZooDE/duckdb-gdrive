**Findings**

- **High** [e2e/helpers/materialise.py:41](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/materialise.py:41), [test/sql/gdrive_read.test.template:18](/home/jr/Projects/datazoo/duckdb-gdrive/test/sql/gdrive_read.test.template:18), [.github/workflows/Checks.yml:86](/home/jr/Projects/datazoo/duckdb-gdrive/.github/workflows/Checks.yml:86)  
  CI live SQL is wired to `GDRIVE_CI_SA_KEY_B64`, but materialisation only substitutes `${SA_KEY_FILE}` from `GDRIVE_CI_SA_KEY_FILE`, and the SQL test also `require-env`s that file var. In GitHub Actions this can fail before testing anything, despite credentials being present as base64.  
  Minimal fix: `_key_path()` should return/reuse one path and materialise `SA_KEY_FILE` from that path, or `run_live_tests.sh` should decode B64 once, export `GDRIVE_CI_SA_KEY_FILE`, and delete it on exit.

- **High** [e2e/helpers/drive.py:56](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:56), [e2e/helpers/drive.py:58](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:58), [e2e/helpers/drive.py:62](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:62), [.env.gdrive.example:18](/home/jr/Projects/datazoo/duckdb-gdrive/.env.gdrive.example:18)  
  The base64 service-account key is written to a 0600 temp file, but never deleted. `credentials_available()` also calls `_key_path()` and leaks a temp key before `Drive.from_env()` creates another one. If the process dies, the JSON key remains on disk. The example file claims it is deleted on exit, but it is not.  
  Minimal fix: decode once into a `NamedTemporaryFile(delete=False)` owned by a fixture/context manager, export the path for children, register `atexit` cleanup, and unlink in `finally`; document that crash cleanup is best-effort.

- **High** [src/include/gdrive_filesystem.hpp:31](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:31), [src/include/gdrive_filesystem.hpp:38](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:38), [src/include/gdrive_filesystem.hpp:157](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:157)  
  The cache is described as “per connection” but is a member of the registered filesystem object. That object is installed once as a subsystem, so the cache can outlive and cross `ClientContext`s/secrets. A path resolved under one secret/root/Shared Drive can be reused under another, violating permissions and correctness.  
  Minimal fix: move resolver/cache state behind per-connection or per-secret state, or key every entry by `{secret_name, drive_id, root_folder_id, canonical_path}` and validate the auth context before hits.

- **High** [e2e/helpers/drive.py:266](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:266), [e2e/helpers/drive.py:268](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:268), [e2e/helpers/drive.py:271](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:271)  
  `find_or_create_folder` is racy. Two concurrent jobs can both list no `/scratch` or `/fixtures`, then both create it. Later code silently picks `existing[0]`, so fixtures and scratch can split across duplicate roots.  
  Minimal fix: after create, re-list and require exactly one folder; if duplicates exist, fail loudly with ids and ask for manual cleanup. For scratch root specifically, seed it once and treat duplicates as harness corruption.

- **Medium** [e2e/helpers/seed.py:44](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/seed.py:44), [e2e/helpers/seed.py:50](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/seed.py:50), [e2e/helpers/seed.py:56](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/seed.py:56)  
  `make seed_fixtures` can corrupt in-flight tests. On checksum drift it deletes the existing permanent fixture before uploading the replacement, creating a window where materialised ids go stale or path reads fail. Duplicate seeding has similar delete/recreate behavior.  
  Minimal fix: never mutate `/fixtures` in place while live tests run. Upload replacement under a versioned staging folder, verify it, then switch a single manifest/root pointer, or require a CI concurrency lock between seeding and live tests.

- **Medium** [e2e/helpers/sweep.py:32](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/sweep.py:32), [e2e/helpers/sweep.py:37](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/sweep.py:37), [e2e/helpers/sweep.py:44](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/sweep.py:44)  
  The 24h cutoff is not intrinsically safe. It keys off folder `modifiedTime`, which may not advance with child activity, and there is no live-run lease. A stalled/long debug run or a scheduled sweep overlapping a slow job can delete an active `run-*`.  
  Minimal fix: create a heartbeat file inside each scratch folder and update it during tests; sweep only folders whose heartbeat is older than the cutoff. For CI, also use a separate sweep credential/job window or a lock file.

- **Medium** [scripts/run_live_tests.sh:52](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/run_live_tests.sh:52), [scripts/run_live_tests.sh:54](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/run_live_tests.sh:54), [scripts/run_live_tests.sh:56](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/run_live_tests.sh:56)  
  `make test_live` can silently pass after testing no SQL if no templates materialise. That is explicitly the false-green mode the script claims to prevent.  
  Minimal fix: exit non-zero when `${#tests[@]} == 0`, or require an explicit `ALLOW_EMPTY_LIVE_SQL=1` during early scaffolding only.

- **Medium** [Makefile:15](/home/jr/Projects/datazoo/duckdb-gdrive/Makefile:15), [Makefile:31](/home/jr/Projects/datazoo/duckdb-gdrive/Makefile:31), [extension-ci-tools/makefiles/duckdb_extension.Makefile:181](/home/jr/Projects/datazoo/duckdb-gdrive/extension-ci-tools/makefiles/duckdb_extension.Makefile:181)  
  `make test` does not run Catch2. It inherits `test: test_release` from extension-ci-tools, while Catch2 is only on `make unit_test`. A no-credential developer may get green SQL while pure logic is broken.  
  Minimal fix: override `test: unit_test test_release` in this Makefile, and keep `test_live` separate.

- **Medium** [scripts/check_no_credentials.sh:37](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/check_no_credentials.sh:37), [scripts/check_no_credentials.sh:43](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/check_no_credentials.sh:43)  
  The credential scanner misses token-shaped secrets and exempts all `*/testdata/*`. The comment says Google refresh tokens start `1//`, but the script does not scan for them. The broad testdata exemption means a real service-account key committed under `test/cpp/testdata/` would pass. The fake key is fine as a test artifact, but the blanket exemption weakens the check.  
  Minimal fix: add refresh/access token patterns, exempt only the exact fake key by path and expected fingerprint, and fail on any other PEM/private key in testdata.

- **Medium** [src/gdrive_errors.cpp:181](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_errors.cpp:181), [src/gdrive_errors.cpp:196](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_errors.cpp:196), [src/gdrive_errors.cpp:301](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_errors.cpp:301)  
  `RedactCredentials()` is too narrow. This leaks: `refresh token 1//0gREAL_SECRET rejected`, because only exact `Bearer `, `ya29.`, and PEM blocks are redacted. Lowercase/whitespace variants like `bearer\tabc.def` also leak unless the token happens to start `ya29.`.  
  Minimal fix: redact JSON-ish keys such as `access_token`, `refresh_token`, `private_key`, `client_secret`; redact `1//...`; and match `Bearer` case-insensitively with flexible whitespace.

- **Medium** [src/gdrive_uri.cpp:91](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_uri.cpp:91), [src/gdrive_uri.cpp:96](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_uri.cpp:96), [test/cpp/test_uri.cpp:178](/home/jr/Projects/datazoo/duckdb-gdrive/test/cpp/test_uri.cpp:178)  
  `gdrive://id:1a2b3c/more` is accepted as a normal path. That is a wrong parse for a malformed direct-id URI, and it can silently resolve a real folder named `id:1a2b3c`.  
  Minimal fix: if the first segment starts `id:`, require the whole URI to be exactly `gdrive://id:<valid-id>`; otherwise reject with “id form cannot have path segments”.

- **Medium** [src/gdrive_glob.cpp:247](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_glob.cpp:247), [src/gdrive_glob.cpp:296](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_glob.cpp:296), [src/gdrive_glob.cpp:298](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_glob.cpp:298)  
  Brace expansion is unbounded and recursive. A user pattern with repeated alternations can allocate exponential memory before any Drive API call happens. The matcher avoids catastrophic backtracking, but expansion is still a local DoS.  
  Minimal fix: cap expansion count and pattern length, then error with a clear message once exceeded.

- **Low** [src/include/gdrive_filesystem.hpp:126](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:126), [src/include/gdrive_filesystem.hpp:132](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:132), [src/include/gdrive_filesystem.hpp:151](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:151)  
  R-4 is only specified on `ResolveOrThrow`. `ListFiles` and `Glob` return names/paths, not collision metadata. A folder with duplicate names can surface duplicate indistinguishable paths or defer the ambiguity until open.  
  Minimal fix: make `Glob` emit `gdrive://id:<fileId>` for concrete matches, or detect duplicate sibling names during listing and error there too. For `ListFiles`, document and enforce duplicate-name behavior explicitly.

**Direct Answers**

Q1: Concurrent scratch folders mostly avoid per-test collisions, but the harness is not concurrency-safe. `/scratch` and `/fixtures` root creation is racy, seeding can delete/reupload permanent fixtures during reads, and the 24h sweep cutoff is not a real active-run guard.

Q2: Secrets are not intentionally echoed by `setup_ci_drive.sh` or the workflows, and `.gcp-key.json`/`.env.gdrive` are gitignored. But base64 keys leak as undeleted temp files, redaction misses refresh tokens and header variants, and the credential scanner has major blind spots.

Q3: A no-credential developer can get a clean `make test`, but not for the reason promised: `make test` does not run Catch2. CI live gating for fork PRs is correct, but `test_live` can currently pass with zero materialised SQL tests and is broken for B64-only CI credentials.
tokens used
116,348
**Findings**

- **High** [e2e/helpers/materialise.py:41](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/materialise.py:41), [test/sql/gdrive_read.test.template:18](/home/jr/Projects/datazoo/duckdb-gdrive/test/sql/gdrive_read.test.template:18), [.github/workflows/Checks.yml:86](/home/jr/Projects/datazoo/duckdb-gdrive/.github/workflows/Checks.yml:86)  
  CI live SQL is wired to `GDRIVE_CI_SA_KEY_B64`, but materialisation only substitutes `${SA_KEY_FILE}` from `GDRIVE_CI_SA_KEY_FILE`, and the SQL test also `require-env`s that file var. In GitHub Actions this can fail before testing anything, despite credentials being present as base64.  
  Minimal fix: `_key_path()` should return/reuse one path and materialise `SA_KEY_FILE` from that path, or `run_live_tests.sh` should decode B64 once, export `GDRIVE_CI_SA_KEY_FILE`, and delete it on exit.

- **High** [e2e/helpers/drive.py:56](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:56), [e2e/helpers/drive.py:58](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:58), [e2e/helpers/drive.py:62](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:62), [.env.gdrive.example:18](/home/jr/Projects/datazoo/duckdb-gdrive/.env.gdrive.example:18)  
  The base64 service-account key is written to a 0600 temp file, but never deleted. `credentials_available()` also calls `_key_path()` and leaks a temp key before `Drive.from_env()` creates another one. If the process dies, the JSON key remains on disk. The example file claims it is deleted on exit, but it is not.  
  Minimal fix: decode once into a `NamedTemporaryFile(delete=False)` owned by a fixture/context manager, export the path for children, register `atexit` cleanup, and unlink in `finally`; document that crash cleanup is best-effort.

- **High** [src/include/gdrive_filesystem.hpp:31](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:31), [src/include/gdrive_filesystem.hpp:38](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:38), [src/include/gdrive_filesystem.hpp:157](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:157)  
  The cache is described as “per connection” but is a member of the registered filesystem object. That object is installed once as a subsystem, so the cache can outlive and cross `ClientContext`s/secrets. A path resolved under one secret/root/Shared Drive can be reused under another, violating permissions and correctness.  
  Minimal fix: move resolver/cache state behind per-connection or per-secret state, or key every entry by `{secret_name, drive_id, root_folder_id, canonical_path}` and validate the auth context before hits.

- **High** [e2e/helpers/drive.py:266](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:266), [e2e/helpers/drive.py:268](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:268), [e2e/helpers/drive.py:271](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/drive.py:271)  
  `find_or_create_folder` is racy. Two concurrent jobs can both list no `/scratch` or `/fixtures`, then both create it. Later code silently picks `existing[0]`, so fixtures and scratch can split across duplicate roots.  
  Minimal fix: after create, re-list and require exactly one folder; if duplicates exist, fail loudly with ids and ask for manual cleanup. For scratch root specifically, seed it once and treat duplicates as harness corruption.

- **Medium** [e2e/helpers/seed.py:44](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/seed.py:44), [e2e/helpers/seed.py:50](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/seed.py:50), [e2e/helpers/seed.py:56](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/seed.py:56)  
  `make seed_fixtures` can corrupt in-flight tests. On checksum drift it deletes the existing permanent fixture before uploading the replacement, creating a window where materialised ids go stale or path reads fail. Duplicate seeding has similar delete/recreate behavior.  
  Minimal fix: never mutate `/fixtures` in place while live tests run. Upload replacement under a versioned staging folder, verify it, then switch a single manifest/root pointer, or require a CI concurrency lock between seeding and live tests.

- **Medium** [e2e/helpers/sweep.py:32](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/sweep.py:32), [e2e/helpers/sweep.py:37](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/sweep.py:37), [e2e/helpers/sweep.py:44](/home/jr/Projects/datazoo/duckdb-gdrive/e2e/helpers/sweep.py:44)  
  The 24h cutoff is not intrinsically safe. It keys off folder `modifiedTime`, which may not advance with child activity, and there is no live-run lease. A stalled/long debug run or a scheduled sweep overlapping a slow job can delete an active `run-*`.  
  Minimal fix: create a heartbeat file inside each scratch folder and update it during tests; sweep only folders whose heartbeat is older than the cutoff. For CI, also use a separate sweep credential/job window or a lock file.

- **Medium** [scripts/run_live_tests.sh:52](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/run_live_tests.sh:52), [scripts/run_live_tests.sh:54](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/run_live_tests.sh:54), [scripts/run_live_tests.sh:56](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/run_live_tests.sh:56)  
  `make test_live` can silently pass after testing no SQL if no templates materialise. That is explicitly the false-green mode the script claims to prevent.  
  Minimal fix: exit non-zero when `${#tests[@]} == 0`, or require an explicit `ALLOW_EMPTY_LIVE_SQL=1` during early scaffolding only.

- **Medium** [Makefile:15](/home/jr/Projects/datazoo/duckdb-gdrive/Makefile:15), [Makefile:31](/home/jr/Projects/datazoo/duckdb-gdrive/Makefile:31), [extension-ci-tools/makefiles/duckdb_extension.Makefile:181](/home/jr/Projects/datazoo/duckdb-gdrive/extension-ci-tools/makefiles/duckdb_extension.Makefile:181)  
  `make test` does not run Catch2. It inherits `test: test_release` from extension-ci-tools, while Catch2 is only on `make unit_test`. A no-credential developer may get green SQL while pure logic is broken.  
  Minimal fix: override `test: unit_test test_release` in this Makefile, and keep `test_live` separate.

- **Medium** [scripts/check_no_credentials.sh:37](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/check_no_credentials.sh:37), [scripts/check_no_credentials.sh:43](/home/jr/Projects/datazoo/duckdb-gdrive/scripts/check_no_credentials.sh:43)  
  The credential scanner misses token-shaped secrets and exempts all `*/testdata/*`. The comment says Google refresh tokens start `1//`, but the script does not scan for them. The broad testdata exemption means a real service-account key committed under `test/cpp/testdata/` would pass. The fake key is fine as a test artifact, but the blanket exemption weakens the check.  
  Minimal fix: add refresh/access token patterns, exempt only the exact fake key by path and expected fingerprint, and fail on any other PEM/private key in testdata.

- **Medium** [src/gdrive_errors.cpp:181](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_errors.cpp:181), [src/gdrive_errors.cpp:196](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_errors.cpp:196), [src/gdrive_errors.cpp:301](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_errors.cpp:301)  
  `RedactCredentials()` is too narrow. This leaks: `refresh token 1//0gREAL_SECRET rejected`, because only exact `Bearer `, `ya29.`, and PEM blocks are redacted. Lowercase/whitespace variants like `bearer\tabc.def` also leak unless the token happens to start `ya29.`.  
  Minimal fix: redact JSON-ish keys such as `access_token`, `refresh_token`, `private_key`, `client_secret`; redact `1//...`; and match `Bearer` case-insensitively with flexible whitespace.

- **Medium** [src/gdrive_uri.cpp:91](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_uri.cpp:91), [src/gdrive_uri.cpp:96](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_uri.cpp:96), [test/cpp/test_uri.cpp:178](/home/jr/Projects/datazoo/duckdb-gdrive/test/cpp/test_uri.cpp:178)  
  `gdrive://id:1a2b3c/more` is accepted as a normal path. That is a wrong parse for a malformed direct-id URI, and it can silently resolve a real folder named `id:1a2b3c`.  
  Minimal fix: if the first segment starts `id:`, require the whole URI to be exactly `gdrive://id:<valid-id>`; otherwise reject with “id form cannot have path segments”.

- **Medium** [src/gdrive_glob.cpp:247](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_glob.cpp:247), [src/gdrive_glob.cpp:296](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_glob.cpp:296), [src/gdrive_glob.cpp:298](/home/jr/Projects/datazoo/duckdb-gdrive/src/gdrive_glob.cpp:298)  
  Brace expansion is unbounded and recursive. A user pattern with repeated alternations can allocate exponential memory before any Drive API call happens. The matcher avoids catastrophic backtracking, but expansion is still a local DoS.  
  Minimal fix: cap expansion count and pattern length, then error with a clear message once exceeded.

- **Low** [src/include/gdrive_filesystem.hpp:126](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:126), [src/include/gdrive_filesystem.hpp:132](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:132), [src/include/gdrive_filesystem.hpp:151](/home/jr/Projects/datazoo/duckdb-gdrive/src/include/gdrive_filesystem.hpp:151)  
  R-4 is only specified on `ResolveOrThrow`. `ListFiles` and `Glob` return names/paths, not collision metadata. A folder with duplicate names can surface duplicate indistinguishable paths or defer the ambiguity until open.  
  Minimal fix: make `Glob` emit `gdrive://id:<fileId>` for concrete matches, or detect duplicate sibling names during listing and error there too. For `ListFiles`, document and enforce duplicate-name behavior explicitly.

**Direct Answers**

Q1: Concurrent scratch folders mostly avoid per-test collisions, but the harness is not concurrency-safe. `/scratch` and `/fixtures` root creation is racy, seeding can delete/reupload permanent fixtures during reads, and the 24h sweep cutoff is not a real active-run guard.

Q2: Secrets are not intentionally echoed by `setup_ci_drive.sh` or the workflows, and `.gcp-key.json`/`.env.gdrive` are gitignored. But base64 keys leak as undeleted temp files, redaction misses refresh tokens and header variants, and the credential scanner has major blind spots.

Q3: A no-credential developer can get a clean `make test`, but not for the reason promised: `make test` does not run Catch2. CI live gating for fork PRs is correct, but `test_live` can currently pass with zero materialised SQL tests and is broken for B64-only CI credentials.
# Triage — codex review #1

Recorded per plan §3.3: findings become slices, and anything judged wrong is
recorded with the reason rather than silently dropped.

| # | Finding | Verdict | Action |
|---|---|---|---|
| H-1 | CI's base64-only credentials never reach the SQL tests (`materialise.py` reads only `GDRIVE_CI_SA_KEY_FILE`) | **Confirmed.** A CI run with valid secrets would fail or skip — credentials present, nothing tested. | Fixed: `run_live_tests.sh` decodes the b64 once into a 0600 temp file, exports the path, removes it on a trap. |
| H-2 | Decoded temp key never deleted; `credentials_available()` leaks a second copy | **Confirmed.** | Fixed: single module-level temp key, reused, removed via `atexit`. Documented that a hard kill leaves it behind — best-effort is the honest claim. |
| H-3 | Path cache is on the filesystem object, which is registered once and spans every `ClientContext` | **Confirmed, and the most serious finding.** A path resolved under one secret/Shared Drive could be served to a connection holding a different secret. | Fixed in the frozen header before implementation: `CacheKey` now carries secret name, drive id and root folder, and a hit is honoured only when the current auth context matches. |
| H-4 | `find_or_create_folder` is racy; two runs can both create `/scratch` | **Confirmed.** Returning `existing[0]` would hide the split indefinitely. | Fixed: re-list after create and fail loudly on duplicates. The race cannot be prevented without an atomic create-if-absent, which Drive does not offer — so detect, don't pretend. |
| M-1 | `make test_live` passes having materialised zero tests | **Confirmed** — exactly the false green the script claims to prevent. | Fixed: fails unless `ALLOW_EMPTY_LIVE_SQL=1`. |
| M-2 | `make test` never ran Catch2 (`extension-ci-tools` defines `test: test_release`) | **Confirmed**, and `CLAUDE.md` claimed otherwise. | Fixed: `test: unit_test test_release`. |
| M-3 | Credential scanner missed `1//` tokens and exempted all of `testdata/` | **Confirmed.** A real key under `test/cpp/testdata/` would have passed. | Fixed: PEM patterns apply everywhere except one allowlisted throwaway (checked for a `_comment` marker and for not naming a real DataZoo project); token patterns apply outside `test/`. Verified with a planted leak. |
| M-4 | `RedactCredentials()` defeated by `1//…` and `bearer\t…` | **Confirmed.** | Delegated; case-insensitive `Bearer`, `1//`, and JSON secret keys. |
| M-5 | `gdrive://id:x/more` silently parsed as an ordinary path | **Confirmed**, and I now think the original decision was wrong. A user writing that has typoed; silently resolving a folder literally named `id:x` is the worse outcome. | Delegated; reject with an actionable message. Leading position only — `gdrive://folder/id:weird` stays an ordinary segment. |
| M-6 | Brace expansion unbounded → local DoS before any API call | **Confirmed.** | Delegated; capped with an explicit error, never silent truncation (a truncated glob drops files from a result set). |
| M-7 | `seed_fixtures` deletes a fixture before re-uploading, racing live tests | **Confirmed but not fixed.** The suggested staging-and-pointer-swap is real work for a case that only arises if someone seeds while CI runs. | Deferred: documented instead, and the nightly sweep/seed jobs are scheduled not to overlap. Revisit if it ever bites. |
| L-1 | R-4 (duplicate names) enforced only in `ResolveOrThrow`, not `Glob`/`ListFiles` | **Confirmed.** Two siblings sharing a name yield two identical, indistinguishable paths. | Fixed in the header: listings emit the `id:` form for ambiguous entries so every returned path addresses exactly one file. |
