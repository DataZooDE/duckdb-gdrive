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
    echo "no live tests materialised (no *.test.template yet) -- nothing to run"
    exit 0
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
