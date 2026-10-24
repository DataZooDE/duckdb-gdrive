#!/usr/bin/env bash
# Offline test of scripts/check_vfs_parity.sh (no network). It reproduces the
# merge-order race: a paired change lands in both repos, our side merges
# first, and the push-to-main run compares against the sibling's main before
# the sibling's half has merged. Throwaway git repos in a mktemp dir stand in
# for the sibling's origin and its CI clone.
#
#   1. without --wait the stale sibling fails at once (current behaviour);
#   2. with --wait the check passes once the sibling's main catches up;
#   3. with --wait real drift (the sibling never catches up) still FAILS.
#
# Usage: scripts/test_vfs_parity.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OURS_NAME="gdrive_vfs_functions.cpp"
THEIRS_NAME="sharepoint_vfs_functions.cpp"

T="$(mktemp -d)"
BG=""
cleanup() { [[ -n "$BG" ]] && kill "$BG" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

g() { git -c user.name=test -c user.email=test@example.invalid -c init.defaultBranch=main "$@"; }
content() { printf '// KEEP THIS FILE IDENTICAL in duckdb-sharepoint and duckdb-gdrive\n// version %s\n' "$1"; }

# Our tree: the script under test plus the NEW shared file.
mkdir -p "$T/ours/scripts" "$T/ours/src"
cp scripts/check_vfs_parity.sh "$T/ours/scripts/"
content new > "$T/ours/src/$OURS_NAME"

# A sibling "origin" whose main still has the OLD file, and a CI-style clone.
new_sibling() { # $1 = name
    mkdir -p "$T/$1-origin/src"
    content old > "$T/$1-origin/src/$THEIRS_NAME"
    g -C "$T/$1-origin" init -q
    g -C "$T/$1-origin" add -A
    g -C "$T/$1-origin" commit -q -m old
    g clone -q "$T/$1-origin" "$T/$1"
}
catch_up() { # $1 = name: the sibling's half of the paired change merges
    content new > "$T/$1-origin/src/$THEIRS_NAME"
    g -C "$T/$1-origin" commit -q -am new
}

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
# Run check_vfs_parity.sh (short poll interval), output to $T/log. Succeeds
# only on a real "OK:" -- a SKIP exits 0 too, but compared nothing.
run() {
    VFS_PARITY_POLL=1 "$T/ours/scripts/check_vfs_parity.sh" "$@" >"$T/log" 2>&1 \
        && grep -q '^OK:' "$T/log"
}
# Fails only on a real mismatch: non-zero exit AND the FAIL diagnosis.
run_fails() {
    ! VFS_PARITY_POLL=1 "$T/ours/scripts/check_vfs_parity.sh" "$@" >"$T/log" 2>&1 \
        && grep -q '^FAIL: the shared VFS functions differ' "$T/log"
}

# 1. Immediate check against the stale sibling fails.
new_sibling race
if run_fails "$T/race"; then
    pass "stale sibling without --wait fails immediately"
else
    fail "stale sibling without --wait did not fail on the mismatch"; cat "$T/log"
fi

# 2. With --wait, the sibling's main catches up after ~3 s -> passes.
( sleep 3; catch_up race ) &
BG=$!
start=$SECONDS
if run --wait 30 "$T/race"; then
    pass "--wait 30 passes once the sibling catches up ($((SECONDS - start)) s)"
else
    fail "--wait 30 did not pass after the sibling caught up"; cat "$T/log"
fi
wait "$BG" 2>/dev/null || true
BG=""

# 3. Real drift: the sibling never catches up -> --wait 5 still fails.
new_sibling drift
start=$SECONDS
if run_fails --wait 5 "$T/drift" && (( SECONDS - start >= 4 )); then
    pass "--wait 5 still fails on real drift ($((SECONDS - start)) s)"
else
    fail "--wait 5 did not wait and then fail on real drift"; cat "$T/log"
fi

# 4. The sibling path may also come before the option (race has caught up).
if run "$T/race" --wait 5; then
    pass "sibling path before --wait is accepted"
else
    fail "sibling path before --wait rejected"; cat "$T/log"
fi

if (( FAILS )); then
    echo "$FAILS check(s) failed" >&2
    exit 1
fi
echo "OK: all check_vfs_parity.sh cases passed"
