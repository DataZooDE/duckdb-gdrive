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
content_patterns=(
    '"private_key":[[:space:]]*"-----BEGIN'
    '-----BEGIN [A-Z ]*PRIVATE KEY-----'
)
while IFS= read -r f; do
    case "$f" in
        *.example|*/testdata/*|scripts/check_no_credentials.sh) continue ;;
    esac
    [[ -f "$f" ]] || continue
    for cp in "${content_patterns[@]}"; do
        if grep -qE "$cp" "$f" 2>/dev/null; then
            echo "FAIL: $f contains credential-shaped content (/$cp/)" >&2
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
