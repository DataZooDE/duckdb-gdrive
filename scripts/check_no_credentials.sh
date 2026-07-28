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
# "ya29.".
#
# A few files must be allowed to contain token-SHAPED literals, because the
# REQ-NF-03 redaction tests prove the redactor strips them and cannot do that
# without one. Those are allowlisted BY EXACT PATH below.
#
# This used to exempt ALL of test/, which is far too broad and was proven so:
# a harness generating test/sql/live/ducklake_conf/*.test wrote a REAL refresh
# token into a tracked-able file and this scanner said "OK: no credentials
# tracked". An exemption wide enough to cover a directory tree is wide enough
# to hide a real credential in it.
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

# Files permitted to contain PEM-SHAPED placeholders. Asserting that a
# malformed key file yields the right error needs a fixture that LOOKS like a
# key, so a flat ban would forbid testing the control this script enforces.
# Allowlisted BY EXACT PATH, never by directory: a blanket test/ exemption
# would let a real key be committed under test/ and pass silently.
# Files permitted to contain token-SHAPED literals. By exact path, never by
# directory -- see the note above token_patterns for what a directory-wide
# exemption actually hides.
TOKEN_ALLOWED=(
    "test/cpp/test_errors.cpp"
    "test/sql/gdrive_secret.test"
    # Contains "client_secret": "not-a-real-secret" -- a placeholder proving
    # the parser reads the field. Reviewed when the exemption was narrowed
    # from all of test/ to these three paths.
    "test/cpp/test_service_account.cpp"
)

PEM_ALLOWED=(
    "$ALLOWED_KEY_FILE"
    "test/sql/gdrive_secret.test"
    # Asserts FormatUserMessage() redacts a PEM block out of a Drive error
    # body (REQ-NF-03). Proving the redactor strips a key requires a key to
    # strip. The literal is a truncated, non-functional base64 body.
    "test/cpp/test_errors.cpp"
)

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

# ---------------------------------------------------------------------------
# Match one pattern against one file.
#
# Two things here are not incidental, and both have already bitten:
#
#   -e "$pat" -- "$file"
#       A PEM pattern begins with `-----`. Written as `grep -qE "$pat" "$f"`,
#       grep parses it as a bundle of short options, prints usage, and exits
#       2. `if grep ...; then` reads a non-zero exit as "no match", so the
#       check passed on a file containing a private key. It did that from the
#       day it was written until a planted-key negative control caught it.
#
#   exit status >= 2 is an ERROR, not a miss
#       That is precisely how the above hid. A scanner that cannot read a
#       file must say so, never shrug and return clean.
# ---------------------------------------------------------------------------
pattern_matches() {
    local pat="$1" file="$2" status=0
    grep -qE -e "$pat" -- "$file" 2>/dev/null || status=$?
    if [[ $status -eq 0 ]]; then
        return 0
    fi
    if [[ $status -ge 2 ]]; then
        echo "FAIL: grep errored (status $status) on $file for /$pat/" >&2
        fail=1
    fi
    return 1
}

# ---------------------------------------------------------------------------
# Self-test: prove every pattern still matches something it is meant to catch.
#
# The failure this guards against is not a false alarm, it is SILENCE -- a
# pattern that matches nothing, forever, while the check reports "OK". That
# happened here (see pattern_matches above) and survived because the only
# evidence of a working scanner was a green line that a broken scanner also
# prints. So: plant a known-positive for each pattern and require a hit.
#
# Runs on every invocation. It costs milliseconds and it is the only thing
# standing between "OK: no credentials tracked" and a lie.
# ---------------------------------------------------------------------------
self_test() {
    local probe rc=0
    probe="$(mktemp)"
    # Each entry: <pattern-array-name>|<index>|<line that MUST match>
    local cases=(
        'pem|0|  "private_key": "-----BEGIN PRIVATE KEY-----\nAAAA"'
        'pem|1|-----BEGIN RSA PRIVATE KEY-----'
        'token|0|refresh = 1//0eXaMpLeToKeNvAlUe123456789'
        'token|1|bearer ya29.a0AfB_bYcExAmPlEtOkEn123456789'
        'token|2|  "refresh_token": "0123456789abcdefghij"'
    )
    local c which idx line pat
    for c in "${cases[@]}"; do
        which="${c%%|*}"; c="${c#*|}"
        idx="${c%%|*}"; line="${c#*|}"
        if [[ "$which" == pem ]]; then pat="${pem_patterns[$idx]}"; else pat="${token_patterns[$idx]}"; fi
        printf '%s\n' "$line" > "$probe"
        if ! grep -qE -e "$pat" -- "$probe" 2>/dev/null; then
            echo "FAIL: self-test -- pattern /$pat/ did not match its own known-positive:" >&2
            echo "      $line" >&2
            rc=1
        fi
    done
    # And a known-NEGATIVE, so a pattern degenerating to "match everything"
    # (which would also make the self-test above pass) is caught too.
    printf 'ordinary prose with no secrets in it whatsoever\n' > "$probe"
    for pat in "${pem_patterns[@]}" "${token_patterns[@]}"; do
        if grep -qE -e "$pat" -- "$probe" 2>/dev/null; then
            echo "FAIL: self-test -- pattern /$pat/ matches innocuous text." >&2
            rc=1
        fi
    done
    rm -f "$probe"
    return "$rc"
}

if ! self_test; then
    echo "" >&2
    echo "The credential scanner is not working. Fix it before trusting a pass." >&2
    exit 1
fi

# Tracked files PLUS untracked-but-not-ignored ones.
#
# `git ls-files` alone sees only the index, so a brand-new file is invisible
# until `git add`. The natural workflow -- run the check, then add, then
# commit -- therefore skipped exactly the files most likely to contain a
# freshly pasted credential. That is not hypothetical: a PEM-shaped literal
# in a new test file passed this check and reached a push, and was only
# caught on the NEXT run once the file was tracked.
#
# --exclude-standard keeps .gitignore honoured, so .env.gdrive and build
# output stay out.
scannable_files() {
    {
        git ls-files
        git ls-files --others --exclude-standard
    } | sort -u
}

while IFS= read -r f; do
    case "$f" in
        *.example|scripts/check_no_credentials.sh) continue ;;
    esac
    [[ -f "$f" ]] || continue

    pem_exempt=0
    for allowed in "${PEM_ALLOWED[@]}"; do
        [[ "$f" == "$allowed" ]] && pem_exempt=1
    done
    if [[ $pem_exempt -eq 0 ]]; then
        for cp in "${pem_patterns[@]}"; do
            if pattern_matches "$cp" "$f"; then
                echo "FAIL: $f contains private-key material (/$cp/)" >&2
                fail=1
            fi
        done
    fi

    # Token-shaped literals are allowed ONLY in these exact files.
    token_exempt=0
    for allowed in "${TOKEN_ALLOWED[@]}"; do
        [[ "$f" == "$allowed" ]] && token_exempt=1
    done
    if [[ $token_exempt -eq 1 ]]; then
        continue
    fi
    for cp in "${token_patterns[@]}"; do
        if pattern_matches "$cp" "$f"; then
            echo "FAIL: $f contains a token-shaped secret (/$cp/)" >&2
            fail=1
        fi
    done
done < <(scannable_files)

if [[ $fail -ne 0 ]]; then
    echo "" >&2
    echo "Credential hygiene check FAILED. If a key was committed, rotate it --" >&2
    echo "removing the file is not enough once it is in git history." >&2
    exit 1
fi

echo "OK: no credentials tracked"
