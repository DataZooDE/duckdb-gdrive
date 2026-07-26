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

# The DuckDB version lives in the extension's 512-byte metadata footer,
# which is 8 fixed 32-byte fields:
#
#   [3] ABI type          e.g. "CPP"
#   [4] EXTENSION version e.g. "86e280c" locally, "v0.0.1" in CI (this repo
#                         has no tags, so DuckDB substitutes a dummy)
#   [5] DUCKDB version    <- the one that decides whether it loads
#   [6] platform          e.g. "linux_amd64"
#   [7] metadata version
#
# Parse the FIELD. The first version of this grepped the footer for the first
# version-shaped string and matched field[4] -- the extension's own dummy
# "v0.0.1" -- so it failed in CI on a perfectly correct artifact while
# passing locally, where field[4] happens to be a commit hash. Grepping a
# binary for "something that looks right" is how you get a check that is
# wrong in exactly the environment you built it for.
read -r ext_version duckdb_version platform <<<"$(python3 - "$EXT" <<'PYEOF'
import sys, pathlib
data = pathlib.Path(sys.argv[1]).read_bytes()[-512:]
fields = [data[i * 32:(i + 1) * 32].rstrip(b"\x00").decode("utf-8", "replace") for i in range(8)]
print(fields[4] or "-", fields[5] or "-", fields[6] or "-")
PYEOF
)"

stamp="$duckdb_version"

if [[ "$stamp" != "$WANT_VERSION" ]]; then
    cat >&2 <<MSG
FAIL: $EXT is built for DuckDB $stamp, but we target $WANT_VERSION.

    extension version : $ext_version
    duckdb version    : $stamp
    platform          : $platform

A commit-hash duckdb version means CMake computed it from a submodule that
was not on a release tag -- usually a STALE build directory cached from
before the submodule moved. The artifact will refuse to load anywhere. Fix:

    ./scripts/check_duckdb_pin.sh    # confirm the submodule is right first
    rm -rf build/release && GEN=ninja make

If the target genuinely changed, update WANT_VERSION here, WANT_SHA in
scripts/check_duckdb_pin.sh, the workflow and CLAUDE.md together.
MSG
    exit 1
fi

echo "OK: extension built for DuckDB $stamp ($platform, extension version $ext_version)"
