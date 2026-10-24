#!/usr/bin/env bash
# The scheme-generic write_blob / remove_file / move_file / file_size are
# registered by BOTH duckdb-gdrive and duckdb-sharepoint with
# IGNORE_ON_CONFLICT, so whichever extension loads first provides them. If the
# two copies drift, behaviour silently depends on load order. This compares
# them, normalising only the extension's own name.
#
# Usage: scripts/check_vfs_parity.sh [--wait SECONDS] [path-to-sibling-repo]
#        (default sibling ../duckdb-sharepoint)
#
# --wait SECONDS: on a mismatch, keep polling the sibling (a git checkout)
# for up to SECONDS -- fetch its origin, reset to origin/<its branch>,
# re-compare -- before failing. For the push-to-main run: a paired change
# merges in one repo first, and the sibling's main catches up minutes later.
# Real drift still fails once the time is up. Poll interval: VFS_PARITY_POLL
# seconds (default 20).
set -euo pipefail
cd "$(dirname "$0")/.."

WAIT=0
SIBLING=""
while (( $# )); do
    case "$1" in
        --wait)
            [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]] || { echo "usage: $0 [--wait SECONDS] [sibling]" >&2; exit 2; }
            WAIT="$2"; shift 2 ;;
        --wait=*)
            WAIT="${1#--wait=}"
            [[ "$WAIT" =~ ^[0-9]+$ ]] || { echo "usage: $0 [--wait SECONDS] [sibling]" >&2; exit 2; }
            shift ;;
        -*) echo "usage: $0 [--wait SECONDS] [sibling]" >&2; exit 2 ;;
        *) SIBLING="$1"; shift ;;
    esac
done
SIBLING="${SIBLING:-../duckdb-sharepoint}"
POLL="${VFS_PARITY_POLL:-20}"
OURS="src/gdrive_vfs_functions.cpp"
THEIRS="$SIBLING/src/sharepoint_vfs_functions.cpp"
if [[ ! -f "$THEIRS" ]]; then
    echo "SKIP: $THEIRS not found (pass the sibling checkout as an argument)"
    exit 0
fi
# Until the sibling has adopted the shared, identical copy (its file carries
# the KEEP THIS FILE IDENTICAL marker), there is nothing to compare against.
if ! grep -q "KEEP THIS FILE IDENTICAL" "$THEIRS"; then
    echo "SKIP: $THEIRS predates the shared implementation"
    exit 0
fi
normalise() { sed -e 's/gdrive/EXT/g; s/sharepoint/EXT/g' "$1"; }
same() { diff -q <(normalise "$THEIRS") <(normalise "$OURS") >/dev/null; }

if (( WAIT > 0 )) && ! same; then
    BRANCH="$(git -C "$SIBLING" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
    if [[ -z "$BRANCH" || "$BRANCH" == "HEAD" ]]; then
        echo "note: $SIBLING is not on a branch; cannot poll it" >&2
    elif [[ -n "$(git -C "$SIBLING" status --porcelain)" ]]; then
        # Polling resets the sibling hard; never on a checkout with local work.
        echo "note: $SIBLING has uncommitted changes; not polling it" >&2
    else
        deadline=$((SECONDS + WAIT))
        while ! same && (( SECONDS < deadline )); do
            left=$((deadline - SECONDS))
            echo "mismatch; waiting for $SIBLING origin/$BRANCH to catch up (${left} s left)"
            sleep $(( POLL < left ? POLL : left ))
            git -C "$SIBLING" fetch -q origin \
                && git -C "$SIBLING" reset -q --hard "origin/$BRANCH" \
                || echo "note: refreshing $SIBLING failed; retrying" >&2
        done
        echo "sibling at $(git -C "$SIBLING" rev-parse --short HEAD)"
    fi
fi
if ! diff -u <(normalise "$THEIRS") <(normalise "$OURS"); then
    echo "FAIL: the shared VFS functions differ from $THEIRS" >&2
    exit 1
fi
echo "OK: shared VFS functions identical to $THEIRS"
