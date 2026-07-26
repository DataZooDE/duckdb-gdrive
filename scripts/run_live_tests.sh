#!/usr/bin/env bash
# Run the live SQL suite against REAL Google Drive.
#
# The difference from `make test`: this FAILS when credentials are missing.
# `make test` lets the live tests skip, so a developer without a Google
# account still gets a clean run. In CI a silent skip is a false green, which
# is worse than a red -- so this exists.
#
# Fixture ids are not known until runtime, so *.test.template files are
# materialised into test/sql/live/ with the real ids substituted, then handed
# to DuckDB's unittest runner. Same approach as ../quack-oauth's
# run_integration_keycloak.sh.
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -f .env.gdrive ]]; then
    set -a
    # shellcheck disable=SC1091
    source .env.gdrive
    set +a
fi

if [[ -z "${GDRIVE_CI_DRIVE_ID:-}" ]]; then
    cat >&2 <<'MSG'
FAIL: GDRIVE_CI_DRIVE_ID is not set.

The live suite runs against real Google Drive; there is no fake and no
replay (plan decision D-1). Set up credentials with:

    ./scripts/setup_ci_drive.sh

or copy .env.gdrive.example to .env.gdrive and fill it in.
MSG
    exit 1
fi

if [[ -z "${GDRIVE_CI_SA_KEY_FILE:-}" && -z "${GDRIVE_CI_SA_KEY_B64:-}" ]]; then
    echo "FAIL: neither GDRIVE_CI_SA_KEY_FILE nor GDRIVE_CI_SA_KEY_B64 is set." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# CI supplies the key as base64 (a GitHub secret), but the SQL tests need a
# PATH: the templates substitute ${SA_KEY_FILE} and each test `require-env`s
# GDRIVE_CI_SA_KEY_FILE. Without this decode, a CI run with perfectly good
# credentials fails or skips -- credentials present, nothing tested.
#
# Decode once here, into a 0600 file removed on exit by the trap below, and
# export the path so both the materialiser and the duckdb CLI see it.
# ---------------------------------------------------------------------------
DECODED_KEY=""
cleanup() {
    if [[ -n "$DECODED_KEY" && -f "$DECODED_KEY" ]]; then
        rm -f "$DECODED_KEY"
    fi
}
trap cleanup EXIT INT TERM

if [[ -z "${GDRIVE_CI_SA_KEY_FILE:-}" ]]; then
    DECODED_KEY="$(mktemp -t gdrive-ci-key-XXXXXX.json)"
    chmod 600 "$DECODED_KEY"
    printf '%s' "$GDRIVE_CI_SA_KEY_B64" | base64 -d > "$DECODED_KEY"
    if ! grep -q '"private_key"' "$DECODED_KEY"; then
        echo "FAIL: GDRIVE_CI_SA_KEY_B64 did not decode to a service-account key." >&2
        exit 1
    fi
    export GDRIVE_CI_SA_KEY_FILE="$DECODED_KEY"
    echo "==> decoded service-account key from GDRIVE_CI_SA_KEY_B64"
fi

UNITTEST=build/release/test/unittest
if [[ ! -x "$UNITTEST" ]]; then
    echo "FAIL: $UNITTEST not built. Run \`make\` first." >&2
    exit 1
fi

echo "==> materialising live tests with real fixture ids"
(cd e2e && uv run --frozen python -m helpers.materialise)

shopt -s nullglob
tests=(test/sql/live/*.test)
if [[ ${#tests[@]} -eq 0 ]]; then
    # Passing here would be the exact false green this script exists to
    # prevent: credentials present, suite "green", nothing tested. During
    # early scaffolding (before any *.test.template exists) that is expected,
    # so it is opt-out rather than an unconditional failure -- but it must be
    # opt-out, never the default.
    if [[ "${ALLOW_EMPTY_LIVE_SQL:-}" == "1" ]]; then
        echo "WARNING: no live tests materialised; ALLOW_EMPTY_LIVE_SQL=1 so not failing."
        exit 0
    fi
    echo "FAIL: no live tests materialised from test/sql/*.test.template." >&2
    echo "Credentials are configured, so a pass here would mean nothing was tested." >&2
    echo "Set ALLOW_EMPTY_LIVE_SQL=1 only while scaffolding." >&2
    exit 1
fi

echo "==> running ${#tests[@]} live test file(s)"
failed=0
for t in "${tests[@]}"; do
    echo "--- $t"
    if ! "$UNITTEST" "$t"; then
        failed=1
    fi
done

exit "$failed"
