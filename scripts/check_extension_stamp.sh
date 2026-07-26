#!/usr/bin/env bash
# The built extension must be stamped for the DuckDB version we target.
#
# Why. A DuckDB extension records the exact DuckDB version it was built for,
# and refuses to load into any other:
#
#   Failed to load 'gdrive.duckdb_extension', The file was built specifically
#   for DuckDB version 'd8cdaa33fd' and can only be loaded with that version
#   of DuckDB. (this version of DuckDB is 'v1.5.5')
#
# That is a real message from this repo. The build directory had been
# configured while the duckdb submodule was still drifted onto main, CMake
# CACHED the version it computed then, and every later `make` produced an
# artifact stamped for a commit hash instead of v1.5.5. Everything built,
# every test passed -- because the tests use the statically linked shell,
# which never exercises the loadable artifact at all -- and the thing we
# would have published could not be loaded by anyone.
#
# CLAUDE.md already said `make clean` is needed after bumping the submodule.
# Documentation did not prevent it; this does.
set -euo pipefail

cd "$(dirname "$0")/.."

EXT="${1:-build/release/extension/gdrive/gdrive.duckdb_extension}"

# Keep in step with scripts/check_duckdb_pin.sh and the stable job in
# .github/workflows/MainDistributionPipeline.yml.
WANT_VERSION="v1.5.5"

if [[ ! -f "$EXT" ]]; then
    echo "FAIL: $EXT not built. Run \`make\` first." >&2
    exit 1
fi

# The version lives in the extension's metadata footer, as a plain string in
# the last block of the file.
stamp="$(tail -c 512 "$EXT" | strings | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1 || true)"

if [[ -z "$stamp" ]]; then
    hash_stamp="$(tail -c 512 "$EXT" | strings | grep -E '^[0-9a-f]{10}$' | head -1 || true)"
    cat >&2 <<MSG
FAIL: $EXT is not stamped with a DuckDB RELEASE version.
      Found: ${hash_stamp:-<nothing recognisable>}

A commit-hash stamp means CMake computed the DuckDB version from a submodule
that was not on a release tag -- usually a STALE build directory cached from
before the submodule moved. The artifact will refuse to load into any stock
DuckDB. Fix with:

    ./scripts/check_duckdb_pin.sh    # confirm the submodule is right first
    rm -rf build/release && GEN=ninja make
MSG
    exit 1
fi

if [[ "$stamp" != "$WANT_VERSION" ]]; then
    cat >&2 <<MSG
FAIL: $EXT is stamped $stamp but we target $WANT_VERSION.

    rm -rf build/release && GEN=ninja make

If the target genuinely changed, update WANT_VERSION here, WANT_SHA in
scripts/check_duckdb_pin.sh, the workflow and CLAUDE.md together.
MSG
    exit 1
fi

echo "OK: extension stamped for $stamp"
