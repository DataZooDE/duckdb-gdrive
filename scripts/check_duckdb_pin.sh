#!/usr/bin/env bash
# The duckdb submodule must sit exactly on the tag we ship against.
#
# Why this exists. The submodule had drifted ~9,600 commits past its pinned
# tag onto DuckDB main. Everything was green locally and every release build
# was red, because `BaseSecret::GetName()` returns `const string &` on every
# released 1.4/1.5 version and `const Identifier &` on main. Code written
# against main compiles for the developer and cannot compile for a single
# shipping platform.
#
# That failure mode is silent and expensive: a local green looks exactly as
# convincing as a real one. The only fix is to make the drift itself loud.
#
# Local development builds against the LATEST STABLE. CI additionally builds
# the 1.4 LTS line (see .github/workflows/MainDistributionPipeline.yml) --
# the API differs between the lines, so stable-only local coverage is not
# proof the LTS job will pass.
set -euo pipefail

cd "$(dirname "$0")/.."

# Latest stable. Keep WANT_SHA in step with CLAUDE.md and the `duckdb_version`
# of the stable job in .github/workflows/MainDistributionPipeline.yml.
#
# Pinned by SHA, not by tag name. CI checks submodules out WITHOUT tags, so
# `git rev-parse v1.5.5^{commit}` fails there and the tag-based check failed
# on a perfectly correct tree. The tag is kept only to make messages readable.
WANT_TAG="v1.5.5"
WANT_SHA="d8cdaa33fda8df955cc76ef58a280f68f4cd43fa"

if [[ ! -e duckdb/.git ]]; then
    echo "FAIL: duckdb submodule is not checked out. Run:" >&2
    echo "    git submodule update --init --recursive" >&2
    exit 1
fi

have="$(git -C duckdb rev-parse HEAD)"

if [[ "$have" != "$WANT_SHA" ]]; then
    described="$(git -C duckdb describe --tags 2>/dev/null || echo '<no tags available>')"
    cat >&2 <<MSG
FAIL: duckdb submodule is not on $WANT_TAG.

    want:  $WANT_SHA  ($WANT_TAG)
    have:  $have  ($described)

Build against anything else and a local green proves nothing about the
release matrix -- DuckDB's C++ API changes between versions, so code can
compile here and fail for every shipping platform. Fix with:

    git -C duckdb fetch --tags
    git -C duckdb checkout $WANT_TAG
    git add duckdb

If you are INTENTIONALLY moving to a new DuckDB, update WANT_TAG and WANT_SHA
here, the workflow, and CLAUDE.md together -- and rebuild, because the API
does change between versions.
MSG
    exit 1
fi

# The tag is only checked when it happens to be available (it is not, in CI).
# A mismatch here means WANT_SHA and WANT_TAG have drifted apart in THIS file.
if tag_sha="$(git -C duckdb rev-parse --verify -q "$WANT_TAG^{commit}" 2>/dev/null)"; then
    if [[ "$tag_sha" != "$WANT_SHA" ]]; then
        echo "FAIL: $WANT_TAG resolves to $tag_sha but WANT_SHA says $WANT_SHA." >&2
        echo "      The two constants in this script disagree; fix them together." >&2
        exit 1
    fi
fi

echo "OK: duckdb submodule on $WANT_TAG"
