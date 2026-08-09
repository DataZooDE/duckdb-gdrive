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
# version must match exactly, so a developer without one still gets a green
# run. CI's distribution pipeline runs the equivalent check on every platform.
#
# In CI that leniency is wrong, and the workflow installs a stock CLI of the
# exact version precisely so this script runs for real. But "installed the
# CLI" and "the check ran" were still two different facts: if the install step
# broke, or WANT_VERSION drifted from the version the workflow downloads, both
# SKIP paths exited 0 and the gate passed without loading anything. Silent
# skips are the exact failure mode this file exists to catch, so under CI a
# skip is a failure. Set SMOKE_LOADABLE_STRICT=0 to opt out, or =1 to demand
# the real check locally.
set -euo pipefail

cd "$(dirname "$0")/.."

EXT="$PWD/build/release/extension/gdrive/gdrive.duckdb_extension"
WANT_VERSION="v1.5.5"
STRICT="${SMOKE_LOADABLE_STRICT:-${CI:+1}}"
STRICT="${STRICT:-0}"

# Report a condition that stops the real check from running: fatal under
# STRICT, an honest skip otherwise.
skip_or_fail() {
    if [[ "$STRICT" == "1" ]]; then
        echo "FAIL: $1" >&2
        echo "      Refusing to report success for a check that did not run." >&2
        echo "      (SMOKE_LOADABLE_STRICT=$STRICT)" >&2
        exit 1
    fi
    echo "SKIP: $1"
    exit 0
}

if [[ ! -f "$EXT" ]]; then
    echo "FAIL: $EXT not built. Run \`make\` first." >&2
    exit 1
fi

if ! command -v duckdb >/dev/null 2>&1; then
    skip_or_fail "no stock \`duckdb\` on PATH to load the artifact into."
fi

have="$(duckdb -noheader -list -c "SELECT version();" 2>/dev/null || true)"
if [[ "$have" != "$WANT_VERSION" ]]; then
    # An extension only loads into the exact version it was built for, so a
    # mismatch cannot be worked around here -- but in CI it means the stock
    # CLI the workflow installs has drifted from WANT_VERSION, which is a
    # real defect rather than a missing local tool.
    skip_or_fail "stock duckdb is ${have:-unknown}, artifact targets $WANT_VERSION."
fi

# ---------------------------------------------------------------------------
# When credentials are available, do the REAL thing: load the shipped
# artifact into a stock DuckDB and read from Google Drive with it. That is
# BRD criterion 1 -- "correct against real Drive, clean install, README
# only" -- end to end, in the artifact users install rather than in our
# statically linked development shell.
#
# Without credentials it still verifies loading and symbol resolution, and
# SAYS which of the two it did. A gate that quietly does less is how
# "SKIP: no stock duckdb on PATH" came to count as a pass.
# ---------------------------------------------------------------------------
KEY_FILE="${GDRIVE_CI_SA_KEY_FILE:-}"
DECODED=""
# Must return 0. Under `set -e`, a trap handler whose LAST command fails
# makes the script exit non-zero -- so `[[ cond ]] && rm` returned 1 whenever
# there was nothing to clean up, and the no-credentials path "failed" while
# printing a perfectly happy OK line. Caught by CI; I had checked this
# script's OUTPUT locally and never its exit status.
cleanup() {
    if [[ -n "$DECODED" && -f "$DECODED" ]]; then
        rm -f "$DECODED"
    fi
    return 0
}
trap cleanup EXIT INT TERM

if [[ -z "$KEY_FILE" && -n "${GDRIVE_CI_SA_KEY_B64:-}" ]]; then
    DECODED="$(mktemp -t gdrive-smoke-key-XXXXXX.json)"
    chmod 600 "$DECODED"
    printf '%s' "$GDRIVE_CI_SA_KEY_B64" | base64 -d > "$DECODED"
    KEY_FILE="$DECODED"
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

if [[ -n "$KEY_FILE" && -n "${GDRIVE_CI_DRIVE_ID:-}" ]]; then
    echo "==> reading real Drive data through the shipped artifact"
    live="$(duckdb -unsigned -noheader -list -c "
LOAD '$EXT';
CREATE SECRET smoke (TYPE gdrive, PROVIDER service_account,
    KEY_FILE '$KEY_FILE', ROOT_FOLDER_ID '$GDRIVE_CI_DRIVE_ID',
    DRIVE_SCOPE 'https://www.googleapis.com/auth/drive.readonly');
SELECT 'rows=' || count(*) FROM read_csv('gdrive://fixtures/small.csv');
SELECT 'globbed=' || count(*) FROM glob('gdrive://fixtures/parts/*.parquet');
" 2>&1)"
    if ! grep -q '^rows=5$' <<<"$live" || ! grep -q '^globbed=10$' <<<"$live"; then
        echo "FAIL: the shipped artifact loaded but could not read Drive." >&2
        echo "$live" >&2
        exit 1
    fi
    echo "$live" | grep -E '^(rows|globbed)=' | sed 's/^/    /'
    echo "OK: shipped artifact reads REAL Google Drive from a stock duckdb $have"
else
    echo "OK: shipped artifact loads and resolves in a stock duckdb $have"
    echo "    (no credentials set -- did NOT read Drive through it)"
fi
