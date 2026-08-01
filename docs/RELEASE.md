# Releasing, and submitting to community-extensions

The project's last acceptance criterion is getting into
`duckdb/community-extensions`. That repo builds the extension itself from a
tagged commit and runs **no live tests of ours** — so whatever is broken at
tag time ships. This is the checklist for not finding that out afterwards.

## Definition of done

The eight criteria the project set itself, and where each stands.

| # | Criterion | Status |
|---|---|---|
| 1 | `SELECT count(*) FROM 'gdrive://…parquet'` correct against real Drive, clean install, README only | met — `make smoke_loadable` does exactly this against the shipping artifact |
| 2 | Cold 100 MB Parquet scan within 3× the same file over GCS, number published | **marginal** — 3.05× default, 2.02× tuned; `docs/benchmark.md` |
| 3 | A native Sheet queryable with no manual export | met, live-tested |
| 4 | Credential, permission and quota errors each distinct and actionable | met, except the rate-limit path (see *What must be true first*) |
| 5 | CI green on all target platforms, stable + LTS | met |
| 6 | `datazoo-oauth2` standalone **and `erpl-web` consuming it** with no remaining copy | **open** — deferred by D-2, not dropped |
| 7 | No credential in the repo, in any log, or in any error message | met, gated by `make check_credentials` |
| 8 | Accepted into `duckdb/community-extensions` | submitted 2026-07-30 (PR #2407) |

## Before tagging

Run all of it. Each line is a gate that has caught something real.

```bash
export VCPKG_ROOT=/home/jr/.local/share/vcpkg
export VCPKG_TOOLCHAIN_PATH=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

make check_pin          # submodule on the shipped tag
make check_cstdint      # musl builds too, not just glibc
rm -rf build/release    # MANDATORY if the submodule moved -- see below
GEN=ninja make
make check_stamp        # artifact built for the DuckDB we target
make smoke_loadable     # artifact LOADS into a stock duckdb
make smoke_static       # only platform libraries dynamically linked
make verify_readme      # README <-> code agree in BOTH directions
make latency            # per-request Drive latency; the figures the docs quote
make unit_test
make check_credentials

set -a && . .env.gdrive && set +a
./scripts/run_live_tests.sh
make e2e
make bench              # REQ-NF-01; needs GDRIVE_BENCH_GCS_URI
```

**`rm -rf build/release` is not optional after the submodule moves.** CMake
caches the DuckDB version it computed at configure time. A stale build
directory produced an artifact stamped `d8cdaa33fd` instead of `v1.5.5`,
which no DuckDB will load — while the entire suite stayed green, because
everything except `smoke_loadable` tests the statically linked shell.

Then confirm CI is green on the exact commit you are about to tag —
**both** workflows:

```bash
gh run list --repo DataZooDE/duckdb-gdrive --limit 4
```

`Checks` covers hygiene, musl, the artifact, and the live suite.
`Main Extension Distribution Pipeline` is the one that matters for
publishing: it builds v1.5.5 and v1.4.5 LTS across Linux amd64/arm64, musl,
Windows and both macOS architectures.

## Tagging

The extension's own version comes from `git describe` on THIS repo — but
**not** via DuckDB's own helper, which cannot ever return a tag. Its
`duckdb_extension_generate_version()` runs

    git describe --tags --always --match '${VERSIONING_TAG_MATCH}'

and those single quotes are literal, so git is handed `'v*.*.*'` with the
quotes and matches nothing; `--always` then falls back to the short commit
hash. Measured on this repo at v0.1.0: the quoted form returns `40c9822`, the
unquoted form returns `v0.1.0`.

This section previously claimed that tagging produces `v0.1.0` on the
artifact. It never did, and nothing noticed because `check_extension_stamp.sh`
only validated the DuckDB version field. `extension_config.cmake` now computes
the version itself and passes `EXTENSION_VERSION` explicitly, and the stamp
check asserts it whenever HEAD is exactly on a tag.

### Versioning: CalVer, `vYYYY.MM.DD`

The scheme is **CalVer**, matching every other DataZoo extension (`erpl_*`,
`anofox_*`, `quack_oauth`): the tag is the release date, and there is no
separate version number to keep in step with anything.

Two consequences worth knowing:

* `docs/community-extension-description.yml` carries **no `version:` field**,
  which is also what the sibling descriptors do. That is not an omission.
  `2026.08.01` is not valid semver -- leading zeros are forbidden in numeric
  identifiers -- so a descriptor that declared it could fail validation. The
  tagged `ref` is the version.
* `src/gdrive_version.cpp` holds the string that `SELECT gdrive_version()`
  returns, and it is hardcoded. Nothing compared it with the tag until
  2026-08-01, so the two could drift and the only symptom would be the
  function quietly reporting the wrong release to a user filing a bug.
  `make check_stamp` now fails when HEAD is on a tag and the string is not it.

```bash
git tag -a v2026.08.01 -m "gdrive 2026.08.01"
git push origin v2026.08.01
```

Before tagging, update **both**:

* `src/gdrive_version.cpp`
* the expected value in `test/sql/gdrive_load.test`

## Submitting

1. Copy `docs/community-extension-description.yml` to
   `community-extensions/extensions/gdrive/description.yml`, dropping the
   staging header (it is instructions to us, not content).
2. Set `repo.ref` to the **tagged commit SHA** — not the tag name, and not
   `main`. It is `PLACEHOLDER_SET_AT_RELEASE` on purpose so that submitting
   without thinking about it fails loudly.

   **Validate the SHAPE of what you substituted, not just that the
   placeholder is gone.** On the 0.1.0 submission the SHA was looked up with
   `git rev-list -n1 v0.1.0` while the shell was inside the *community-extensions*
   clone, which has no such tag. The command failed, the variable was empty,
   the placeholder was duly replaced with nothing, and `ref:` parsed as valid
   YAML with a null value. Require 40 hex characters.
3. Open the PR.

**0.1.0 was submitted 2026-07-30 as duckdb/community-extensions#2407**, ref
`181b851`.

## What must be true first

Do not submit while any of these is open. The first two are honesty
obligations: the descriptor's `extended_description` makes claims, and a
reviewer will test them.

**Descriptor status (2026-07-30): clear.** Every claim in `hello_world` was
executed against real Drive, and `extended_description` no longer makes a
positive performance claim — it states that Drive is slower than object
storage and quantifies the ranged-read cost from `make latency`. That is true
whichever way REQ-NF-01 lands, so the descriptor does not block on it. What
still blocks is the decision below.

- **REQ-NF-01: SETTLED 2026-07-30, and it is a marginal miss at the
  defaults.** Both legs in one session, 9 repeats: `gdrive://` 4.09 s against
  GCS 1.34 s = **3.05x**, where the gate is 3x. On medians 2.83x. With
  `gdrive_block_size_bytes` at 128 MiB it is 2.02x, a clear pass.

  This does **not** block submission. `extended_description` makes no positive
  performance claim — it states that Drive is slower than object storage and
  quantifies the ranged-read cost — so it is accurate whichever side of the
  gate the default lands on. The README reports the number and the caveat.

  If you re-measure, use `GDRIVE_BENCH_REPEATS=9` or higher. The GCS leg
  varied 1.03–1.72 s between sessions, which is enough to flip a 3-repeat
  verdict; three of four runs said FAIL and one said PASS on the same code.

- **Retries can duplicate a create.** Drive's simple upload is not
  idempotent: if it commits a `files.create` and the response is lost, a retry
  makes a second file with the same name — and duplicate names in one folder
  are a hard error here (R-4), so a blind retry can poison a path permanently.

  Mitigated: transport failures are retried only for idempotent methods, and
  a failed write says plainly that it may or may not have been applied. Not
  fixed: the real answer is Drive's resumable upload protocol, whose session
  URI makes a retry genuinely idempotent. Documented under *Known limitations*
  in the README so users meet it before it meets them.
- **Rate-limit errors are not covered live.** Deliberate and documented in
  the README; make sure it stays documented rather than quietly dropped.

## Excluded platforms, and why

`windows_amd64_mingw;wasm_mvp;wasm_eh;wasm_threads`.

wasm is not a gap to close later: the extension is HTTPS calls to the Drive
API plus a loopback redirect server for interactive OAuth, and neither works
in a browser sandbox. A wasm side-module defers symbol resolution to load
time, so it would build green in CI and then fail to LOAD. A clean
"unavailable" beats an unloadable artifact.
