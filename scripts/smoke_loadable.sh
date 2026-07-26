#!/usr/bin/env bash
# Load the SHIPPED artifact into a STOCK DuckDB and use it.
#
# Everything else in this repo tests `build/release/duckdb` -- a shell with
# the extension statically linked. That is not what users install. A
# statically linked build can be perfectly green while the loadable artifact
# is unusable, because symbol resolution and the DuckDB-version handshake
# only happen at LOAD time.
#
# That is not hypothetical. A stale CMake cache stamped the artifact for a
# commit hash rather than v1.5.5, and it refused to load into any stock
# DuckDB. Every test passed throughout.
#
# Skips (rather than fails) when no matching stock duckdb is on PATH -- the
# version must match exactly, so we cannot demand one. CI's distribution
# pipeline runs the equivalent check on every platform.
set -euo pipefail

cd "$(dirname "$0")/.."

EXT="$PWD/build/release/extension/gdrive/gdrive.duckdb_extension"
WANT_VERSION="v1.5.5"

if [[ ! -f "$EXT" ]]; then
    echo "FAIL: $EXT not built. Run \`make\` first." >&2
    exit 1
fi

if ! command -v duckdb >/dev/null 2>&1; then
    echo "SKIP: no stock \`duckdb\` on PATH to load the artifact into."
    exit 0
fi

have="$(duckdb -noheader -list -c "SELECT version();" 2>/dev/null || true)"
if [[ "$have" != "$WANT_VERSION" ]]; then
    echo "SKIP: stock duckdb is $have, artifact targets $WANT_VERSION."
    echo "      (An extension only loads into the exact version it was built for.)"
    exit 0
fi

echo "==> loading the shipped artifact into stock duckdb $have"

# Loading alone proves the handshake; calling a function proves the symbols
# actually resolved. `-unsigned` because we do not sign local builds.
out="$(duckdb -unsigned -noheader -list -c "
LOAD '$EXT';
SELECT 'version=' || gdrive_version();
SELECT 'filesystem=' || count(*) FROM duckdb_functions() WHERE function_name = 'gdrive_stats';
" 2>&1)"

if ! grep -q '^version=' <<<"$out"; then
    echo "FAIL: the shipped artifact did not load." >&2
    echo "$out" >&2
    exit 1
fi

echo "$out" | sed 's/^/    /'
echo "OK: shipped artifact loads and resolves in a stock duckdb $have"
