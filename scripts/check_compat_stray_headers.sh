#!/usr/bin/env bash
# Compile the duckdb-compat headers this extension includes (duckdb_compat.hpp) with
# stray DuckDB headers on the include path, the way a machine with a system DuckDB
# package (/usr/include/duckdb) sees it. Ported from anofox-forecast#312.
#
# A version probe such as __has_include("duckdb/common/types/size.hpp") asks
# whether a header exists ANYWHERE on the include path. When the bundled DuckDB
# lacks it, the compiler keeps searching and finds a copy from another DuckDB
# version in the system include directories. This script plants a stub that fails
# with #error for every header that either (a) another supported DuckDB line has
# and the bundled one lacks, or (b) the sources or the duckdb-compat submodule
# probe with __has_include and the bundled one lacks. The stubs go on -isystem,
# which is searched after the bundled tree (-I) and before the real system
# directories, so a stub also wins over a system DuckDB on a developer machine.
# A header that is found there fails the compile.
#
# Usage: scripts/check_compat_stray_headers.sh   (CXX and DUCKDB_INCLUDE override)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
duckdb_include=${DUCKDB_INCLUDE:-$root/duckdb/src/include}
compat_include=$root/duckdb-compat/include
cxx=${CXX:-c++}

# The compat headers to compile: duckdb-compat's umbrella header (it includes
# duckdb_compat/dispatch.hpp).
headers="duckdb_compat.hpp"

if [ ! -f "$duckdb_include/duckdb.hpp" ]; then
	echo "no DuckDB headers at $duckdb_include (git submodule update --init duckdb)" >&2
	exit 2
fi
if [ ! -f "$compat_include/duckdb_compat.hpp" ]; then
	echo "no duckdb-compat headers at $compat_include (git submodule update --init duckdb-compat)" >&2
	exit 2
fi

stray=$(mktemp -d)
tu=$(mktemp -d)
trap 'rm -rf "$stray" "$tu"' EXIT

# (a) Headers of DuckDB 2.0 that the bundled v1.5.6 lacks and that duckdb-compat
#     or this extension once probed for.
known="duckdb/common/types/size.hpp duckdb/planner/table_filter_set.hpp duckdb/common/identifier.hpp"
# (b) Every header the sources or the duckdb-compat submodule probe for.
probed=$(grep -rhoE '__has_include[[:space:]]*\([[:space:]]*["<]duckdb[^">]*[">]' "$root/src" "$compat_include" |
	sed -E 's/.*["<](duckdb[^">]*)[">]$/\1/' || true)

planted=0
for header in $(printf '%s\n' $known $probed | sort -u); do
	if [ ! -e "$duckdb_include/$header" ]; then
		mkdir -p "$stray/$(dirname "$header")"
		printf '#error "stray DuckDB header %s: not in the bundled DuckDB, found on a later include path"\n' \
			"$header" >"$stray/$header"
		echo "planted stray $header"
		planted=$((planted + 1))
	fi
done

if [ "$planted" -eq 0 ]; then
	echo "warning: no stray headers planted; the check below tests nothing" >&2
fi

# The TU lives outside $stray: a quoted include searches the includer's directory first.
for header in $headers; do
	printf '#include "%s"\n' "$header" >>"$tu/tu.cpp"
done
"$cxx" -std=c++17 -fsyntax-only -I"$duckdb_include" -I"$compat_include" -I"$root/src/include" \
	-isystem "$stray" "$tu/tu.cpp"
echo "$headers compile with $planted stray header(s) on the include path"
