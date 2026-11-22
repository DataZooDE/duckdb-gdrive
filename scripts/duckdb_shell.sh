#!/usr/bin/env bash
# build/release/duckdb with THIS extension loaded, whichever DuckDB built it.
#
# Up to DuckDB 1.5 the release shell had the extension statically linked. The
# 2.0 shell no longer links out-of-tree extensions, so every harness that
# drove it (verify_readme, the e2e suite, the bench helpers) saw "Secret type
# not found" and similar. This wrapper loads the built artifact -- unsigned,
# because a local build is -- and does nothing extra when the extension is
# already there (a 1.x build). Arguments and stdin pass straight through.
#
# Usage: scripts/duckdb_shell.sh [duckdb args...]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
ext_name=gdrive
build="${DUCKDB_SHELL_BUILD:-release}"
shell="$root/build/$build/duckdb"
artifact="$root/build/$build/extension/$ext_name/$ext_name.duckdb_extension"

if [[ ! -x "$shell" ]]; then
    echo "duckdb_shell: $shell not built -- run \`make\` first" >&2
    exit 127
fi

have="$("$shell" -noheader -list -c \
    "SELECT count(*) FROM duckdb_extensions() WHERE extension_name = '$ext_name' AND loaded" \
    2>/dev/null </dev/null || true)"
if [[ "$have" == "1" ]]; then
    exec "$shell" "$@"
fi

if [[ ! -f "$artifact" ]]; then
    echo "duckdb_shell: $artifact not built -- run \`make\` first" >&2
    exit 127
fi
exec "$shell" -unsigned -cmd "LOAD '$artifact';" "$@"
