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
make verify_readme      # the README documents things that exist
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

The extension's own version comes from `git describe` on THIS repo. Untagged,
DuckDB substitutes a dummy `v0.0.1` — which is what CI reported for a long
time and is fine there, but is not what you want on a published artifact.

```bash
git tag -a v0.1.0 -m "gdrive 0.1.0"
git push origin v0.1.0
```

Keep the tag in step with `version:` in
`docs/community-extension-description.yml`.

## Submitting

1. Copy `docs/community-extension-description.yml` to
   `community-extensions/extensions/gdrive/description.yml`.
2. Set `repo.ref` to the **tagged commit SHA** — not the tag name, and not
   `main`. It is `PLACEHOLDER_SET_AT_RELEASE` on purpose so that submitting
   without thinking about it fails loudly.
3. Open the PR.

## What must be true first

Do not submit while any of these is open. The first two are honesty
obligations: the descriptor's `extended_description` makes claims, and a
reviewer will test them.

- **REQ-NF-01 is unproven.** `make bench` measures `gdrive://` and local but
  exits non-zero with the 3× gate **NOT EVALUATED**, because there is no GCS
  denominator. Either produce one (see `docs/benchmark.md` → *What is
  missing*) or remove the performance claim from the description.
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
