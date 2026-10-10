#!/usr/bin/env bash
# The scheme-generic write_blob / remove_file / move_file / file_size are
# registered by BOTH duckdb-gdrive and duckdb-sharepoint with
# IGNORE_ON_CONFLICT, so whichever extension loads first provides them. If the
# two copies drift, behaviour silently depends on load order. This compares
# them, normalising only the extension's own name.
#
# Usage: scripts/check_vfs_parity.sh [path-to-sibling-repo]   (default ../duckdb-sharepoint)
set -euo pipefail
cd "$(dirname "$0")/.."

SIBLING="${1:-../duckdb-sharepoint}"
OURS="src/gdrive_vfs_functions.cpp"
THEIRS="$SIBLING/src/sharepoint_vfs_functions.cpp"
if [[ ! -f "$THEIRS" ]]; then
    echo "SKIP: $THEIRS not found (pass the sibling checkout as an argument)"
    exit 0
fi
normalise() { sed -e 's/gdrive/EXT/g; s/sharepoint/EXT/g' "$1"; }
if ! diff -u <(normalise "$THEIRS") <(normalise "$OURS"); then
    echo "FAIL: the shared VFS functions differ from $THEIRS" >&2
    exit 1
fi
echo "OK: shared VFS functions identical to $THEIRS"
