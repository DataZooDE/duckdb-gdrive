# Releasing, and submitting to community-extensions

Criterion 8 of `docs/implementation-plan.md` §6 is acceptance into
`duckdb/community-extensions`. That repo builds the extension itself from a
tagged commit and runs **no live tests of ours** — so whatever is broken at
tag time ships. This is the checklist for not finding that out afterwards.

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

```bash
git tag -a v0.1.0 -m "gdrive 0.1.0"
git push origin v0.1.0
```

Keep the tag in step with `version:` in
`docs/community-extension-description.yml`.

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

- **Retries can duplicate a create.** Mitigated — writes are no longer
  retried after an ambiguous transport failure — but the real fix is Drive's
  resumable upload protocol. See
  `docs/reviews/2026-07-26-codex-review-2-read-path.md`, finding 1.
- **Rate-limit errors are not covered live.** Deliberate and documented in
  the README; make sure it stays documented rather than quietly dropped.

## Excluded platforms, and why

`windows_amd64_mingw;wasm_mvp;wasm_eh;wasm_threads`.

wasm is not a gap to close later: the extension is HTTPS calls to the Drive
API plus a loopback redirect server for interactive OAuth, and neither works
in a browser sandbox. A wasm side-module defers symbol resolution to load
time, so it would build green in CI and then fail to LOAD. A clean
"unavailable" beats an unloadable artifact.
