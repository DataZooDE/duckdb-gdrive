#!/usr/bin/env bash
# The Claude Code plugin's version must be the extension's.
#
# `/plugin marketplace update` compares this version to decide whether a user
# has the newest skills. A release that changes the SQL surface without
# bumping it leaves everyone on the old documentation, silently -- the same
# shape of drift check_stamp catches for the binary. So the plugin manifest
# and every SKILL.md carry the version src/gdrive_version.cpp returns, and the
# release's version-string commit bumps them all together (docs/RELEASE.md).
set -euo pipefail
cd "$(dirname "$0")/.."

want="$(grep -oE '"[0-9]{4}\.[0-9]{2}\.[0-9]{2}"' src/gdrive_version.cpp | head -1 | tr -d '"')"
if [[ -z "$want" ]]; then
    echo "FAIL: no YYYY.MM.DD version string found in src/gdrive_version.cpp" >&2
    exit 1
fi

fail=0
check() { # file, version found
    if [[ "$2" != "$want" ]]; then
        echo "FAIL: $1 has version '${2:-<none>}', the extension is $want" >&2
        fail=1
    fi
}

manifest=plugins/duckdb-gdrive/.claude-plugin/plugin.json
check "$manifest" "$(grep -oE '"version"[[:space:]]*:[[:space:]]*"[^"]*"' "$manifest" | grep -oE '[0-9]{4}\.[0-9]{2}\.[0-9]{2}' || true)"
shopt -s nullglob
skills=(plugins/duckdb-gdrive/skills/*/SKILL.md)
if [[ ${#skills[@]} -eq 0 ]]; then
    echo "FAIL: no skills found under plugins/duckdb-gdrive/skills/" >&2
    exit 1
fi
for f in "${skills[@]}"; do
    check "$f" "$(grep -m1 -E '^version:' "$f" | sed -E 's/^version:[[:space:]]*//' || true)"
done

if [[ $fail -ne 0 ]]; then
    exit 1
fi
echo "OK: plugin manifest and ${#skills[@]} skill(s) at $want"
