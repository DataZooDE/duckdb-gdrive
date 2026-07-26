#!/usr/bin/env bash
# Every file using a fixed-width integer type must include <cstdint>.
#
# Why this is worth a script. glibc's libstdc++ headers pull in <cstdint>
# transitively, so on an ordinary Linux/macOS/Windows build a missing include
# is invisible -- the code compiles everywhere the developer looks. musl's
# headers do not, so the SAME code fails to compile on linux_amd64_musl, which
# is a target in the 1.4 LTS distribution matrix.
#
# That is exactly how it went: `int8_t was not declared in this scope` in
# gdrive_glob.cpp, on musl only, after everything else in the matrix was
# green. Twelve files were relying on the transitive include.
#
# This check costs milliseconds and runs before the ~20-minute matrix build,
# so the feedback arrives in the right order.
set -euo pipefail

cd "$(dirname "$0")/.."

# `size_t` is deliberately NOT in this list: it comes from <cstddef>, which
# std::string/std::vector are all but guaranteed to pull in, and including it
# here would produce noise rather than findings.
FIXED_WIDTH='\b(u?int(8|16|32|64)_t|u?int_fast(8|16|32|64)_t|u?intptr_t)\b'

fail=0
while IFS= read -r f; do
    case "$f" in
        src/*.cpp|src/include/*.hpp|test/cpp/*.cpp) ;;
        *) continue ;;
    esac
    [[ -f "$f" ]] || continue

    # Ignore matches inside // comments -- a comment mentioning int64_t is not
    # a use of it, and flagging one would train people to ignore this check.
    if ! sed 's|//.*||' "$f" | grep -qE "$FIXED_WIDTH"; then
        continue
    fi
    if ! grep -qE '^#include <cstdint>' "$f"; then
        echo "FAIL: $f uses a fixed-width integer type but does not #include <cstdint>" >&2
        fail=1
    fi
done < <(git ls-files)

if [[ $fail -ne 0 ]]; then
    cat >&2 <<'MSG'

This builds fine against glibc and FAILS on musl, which is a platform in the
1.4 LTS distribution matrix. Add `#include <cstdint>` to each file above.
MSG
    exit 1
fi

echo "OK: every fixed-width integer user includes <cstdint>"
