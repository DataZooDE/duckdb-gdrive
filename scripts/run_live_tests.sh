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
SCRATCH_TO_CLEAN=""
cleanup() {
    if [[ -n "$DECODED_KEY" && -f "$DECODED_KEY" ]]; then
        rm -f "$DECODED_KEY"
    fi
    # Delete THIS run's scratch folder. The sweeper is a 24-hour backstop for
    # crashed runs, not teardown: without this, every failed run leaves a tree
    # behind and the noise eventually hides the failure you care about.
    if [[ -n "$SCRATCH_TO_CLEAN" ]]; then
        (cd e2e && uv run --frozen python -m helpers.drop_scratch "$SCRATCH_TO_CLEAN") \
            >/dev/null 2>&1 || true
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

# ---------------------------------------------------------------------------
# DuckLake, for the table-format test.
#
# The unittest binary runs with autoinstall_known_extensions=false, so a
# `require ducklake` inside a .test file SKIPS rather than installs -- and a
# skip in this suite is a false green (see the skip guard at the bottom).
# `LOAD ducklake` works fine there once the extension is actually present, so
# install it here with the shell, which may autoinstall.
# ---------------------------------------------------------------------------
DUCKLAKE_OK=0
DUCKDB_CLI=build/release/duckdb
if [[ -x "$DUCKDB_CLI" ]]; then
    echo "==> ensuring ducklake is installed for the table-format test"
    # Show the error. The first version of this swallowed it with >/dev/null
    # and CI reported "could not install/load ducklake" with no reason, which
    # is the same sin as a silent skip one level along.
    if ducklake_err="$("$DUCKDB_CLI" -c "INSTALL ducklake; LOAD ducklake;" 2>&1)"; then
        DUCKLAKE_OK=1
    else
        echo "$ducklake_err" >&2
    fi
fi

# DuckLake is an UPSTREAM extension we do not build. If the runner cannot
# obtain it, that is an availability problem in the environment, not evidence
# about this extension -- so the DuckLake test is dropped by name, loudly,
# rather than either failing the suite or (worse) silently skipping. Every
# other file still fails on an unexpected skip.
if [[ "$DUCKLAKE_OK" -ne 1 ]]; then
    # The README claims DuckLake works over gdrive://. A run that silently
    # skips the test verifying it lets that claim go unchecked while the suite
    # reports success -- so this is a FAILURE unless someone deliberately
    # waives it. Upstream unavailability is a real situation, which is why the
    # escape hatch exists; it is opt-in so it can never be the default.
    if [[ "${ALLOW_MISSING_DUCKLAKE:-}" == "1" ]]; then
        echo "WARNING: ducklake unavailable; the DuckLake test will NOT run." >&2
        echo "         ALLOW_MISSING_DUCKLAKE=1 is set, so this is not failing --" >&2
        echo "         but DuckLake support is UNVERIFIED in this run." >&2
    else
        echo "FAIL: ducklake could not be installed, so the test proving DuckLake" >&2
        echo "      works over gdrive:// cannot run. Set ALLOW_MISSING_DUCKLAKE=1" >&2
        echo "      to accept an unverified DuckLake claim for this run." >&2
        exit 1
    fi
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

SCRATCH_TO_CLEAN="$(sed -n 's|.*scratch/\(run-[a-z0-9]*\)/.*|\1|p' test/sql/live/*.test 2>/dev/null | head -1)"

echo "==> running ${#tests[@]} live test file(s)"
failed=0
for t in "${tests[@]}"; do
    if [[ "$t" == *ducklake* && "$DUCKLAKE_OK" -ne 1 ]]; then
        echo "--- $t  SKIPPED (ducklake unavailable, see warning above)"
        continue
    fi
    echo "--- $t"
    # Capture as well as show: sqllogictest reports a SKIPPED file as a
    # SUCCESS (exit 0). In a suite whose entire purpose is "prove it works
    # against real Drive", a skip is a false green -- and a silent one,
    # because the summary line reads "All tests were skipped" in the middle
    # of an otherwise green run. A `require <extension>` that cannot be
    # satisfied in the test runner must be a failure here.
    out="$("$UNITTEST" "$t" 2>&1)" || failed=1
    printf '%s\n' "$out"
    if grep -q 'All tests were skipped' <<<"$out"; then
        echo "FAIL: $t was SKIPPED, not run." >&2
        echo "      Credentials are configured, so a skip here tests nothing." >&2
        echo "      Usually an unmet 'require <extension>' -- install it into the" >&2
        echo "      unittest binary's extension directory, or move the test to e2e/." >&2
        failed=1
    fi
done

exit "$failed"
