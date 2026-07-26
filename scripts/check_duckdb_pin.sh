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

# Latest stable. Keep in step with CLAUDE.md and the `duckdb_version` of the
# stable job in .github/workflows/MainDistributionPipeline.yml.
WANT_TAG="v1.5.5"

if [[ ! -e duckdb/.git ]]; then
    echo "FAIL: duckdb submodule is not checked out. Run:" >&2
    echo "    git submodule update --init --recursive" >&2
    exit 1
fi

have="$(git -C duckdb rev-parse HEAD)"
want="$(git -C duckdb rev-parse "$WANT_TAG^{commit}" 2>/dev/null || true)"

if [[ -z "$want" ]]; then
    echo "FAIL: tag $WANT_TAG does not exist in the duckdb submodule." >&2
    echo "      Fetch tags:  git -C duckdb fetch --tags" >&2
    exit 1
fi

if [[ "$have" != "$want" ]]; then
    described="$(git -C duckdb describe --tags 2>/dev/null || echo '<unknown>')"
    cat >&2 <<MSG
FAIL: duckdb submodule is not on $WANT_TAG.

    want:  $want  ($WANT_TAG)
    have:  $have  ($described)

Build against anything else and a local green proves nothing about the
release matrix -- DuckDB's C++ API changes between versions, so code can
compile here and fail for every shipping platform. Fix with:

    git -C duckdb fetch --tags
    git -C duckdb checkout $WANT_TAG
    git add duckdb
MSG
    exit 1
fi

echo "OK: duckdb submodule on $WANT_TAG"
