#!/usr/bin/env bash
# REQ-NF-03 / plan slice S-4.4: no credential is ever committed.
#
# .gitignore protects only people who never ran `git add -f`. This checks what
# git actually TRACKS, so it catches the mistake after it has been made and
# before it ships. Runs in CI on every push.
set -euo pipefail

cd "$(dirname "$0")/.."

fail=0

# ---------------------------------------------------------------------------
# 1. Tracked files matching Google credential-file naming patterns.
# ---------------------------------------------------------------------------
patterns=(
    'client_secret_.*\.json$'
    '.*-[0-9a-f]{12}\.json$'
    '^\.env\.gdrive$'
    '\.gcp-key\.json$'
)
for p in "${patterns[@]}"; do
    hits=$(git ls-files | grep -E "$p" || true)
    if [[ -n "$hits" ]]; then
        echo "FAIL: tracked file(s) match credential pattern /$p/:" >&2
        echo "$hits" >&2
        fail=1
    fi
done

# ---------------------------------------------------------------------------
# 2. Credential-shaped content in tracked files. A service-account JSON always
#    carries these two markers; an OAuth refresh token from Google starts
#    `1//`. Scoped to tracked files so build output and the venv are ignored.
#    `.example` files are exempt -- they exist to show the shape.
# ---------------------------------------------------------------------------
# Private-key material. Applies to EVERY tracked file except the one
# explicitly allowed throwaway below -- there is no legitimate reason for a
# PEM private key to appear anywhere else in this repo.
pem_patterns=(
    '"private_key":[[:space:]]*"-----BEGIN'
    '-----BEGIN [A-Z ]*PRIVATE KEY-----'
)

# Token-shaped strings: Google refresh tokens start "1//", access tokens
# "ya29.". These are NOT applied under test/, because the REQ-NF-03 redaction
# tests must contain token-shaped literals in order to prove the redactor
# strips them -- a scanner that forbade them would forbid testing the very
# control it exists to enforce. Everywhere else they are a hard failure.
token_patterns=(
    '\b1//[A-Za-z0-9_-]{20,}'
    '\bya29\.[A-Za-z0-9_-]{20,}'
    '"(refresh_token|access_token|client_secret)":[[:space:]]*"[^"]{16,}"'
)

# The ONLY file permitted to contain private-key material is the generated
# throwaway used by the unit tests. It is exempted by exact path, not by
# directory: a blanket */testdata/* exemption would let a real service-account
# key be committed under test/cpp/testdata/ and pass this check silently.
ALLOWED_KEY_FILE="test/cpp/testdata/fake_sa_key.json"

if [[ -f "$ALLOWED_KEY_FILE" ]]; then
    if ! grep -q '_comment' "$ALLOWED_KEY_FILE"; then
        echo "FAIL: $ALLOWED_KEY_FILE has no _comment marking it a throwaway." >&2
        echo "If this is a real key, rotate it now -- it is in git history." >&2
        fail=1
    fi
    # A throwaway must not reference a real DataZoo project. Note every
    # service-account key legitimately ends in gserviceaccount.com, so that
    # suffix proves nothing on its own.
    if grep -qE '"(project_id|client_email)":[[:space:]]*"[^"]*(datazoo|data-zoo)' \
            "$ALLOWED_KEY_FILE"; then
        echo "FAIL: $ALLOWED_KEY_FILE references a real DataZoo project." >&2
        fail=1
    fi
fi

while IFS= read -r f; do
    case "$f" in
        *.example|scripts/check_no_credentials.sh) continue ;;
    esac
    [[ -f "$f" ]] || continue

    if [[ "$f" != "$ALLOWED_KEY_FILE" ]]; then
        for cp in "${pem_patterns[@]}"; do
            if grep -qE "$cp" "$f" 2>/dev/null; then
                echo "FAIL: $f contains private-key material (/$cp/)" >&2
                fail=1
            fi
        done
    fi

    case "$f" in
        test/*) continue ;;
    esac
    for cp in "${token_patterns[@]}"; do
        if grep -qE "$cp" "$f" 2>/dev/null; then
            echo "FAIL: $f contains a token-shaped secret (/$cp/)" >&2
            fail=1
        fi
    done
done < <(git ls-files)

if [[ $fail -ne 0 ]]; then
    echo "" >&2
    echo "Credential hygiene check FAILED. If a key was committed, rotate it --" >&2
    echo "removing the file is not enough once it is in git history." >&2
    exit 1
fi

echo "OK: no credentials tracked"
