# Included by DuckDB's build system. Specifies which extension to load.
#
# ---------------------------------------------------------------------------
# Why the C++17 FORCE below is not optional
# ---------------------------------------------------------------------------
# DuckDB caches CMAKE_CXX_STANDARD=11 (duckdb/CMakeLists.txt), and a plain
# `set(... CACHE ...)` in our own CMakeLists is a no-op against an already
# populated cache -- so the whole of DuckDB compiles below C++17 by default.
# That single fact breaks the v1.5.3 build in two different ways:
#
#   * Linux/GCC: linking our extension statically into DuckDB drags
#     posthog_telemetry's PUBLIC cxx_std_17 requirement into DuckDB's own
#     `plan_serializer` tool, so that tool compiles as C++17 while
#     `libduckdb_static` stays C++11. `BufferedFileWriter::DEFAULT_OPEN_FLAGS`
#     is then a COMDAT-weak symbol on one side and a strong out-of-line
#     definition on the other -> "multiple definition" link error.
#   * Windows/MSVC: DuckDB's bundled `fmt` uses inline variables, which MSVC
#     rejects without `/std:c++17` (error C7525). At CMAKE_CXX_STANDARD=11
#     CMake emits no `/std` flag at all on MSVC.
#
# Forcing C++17 for the ENTIRE build fixes both: every TU agrees on C++17, the
# weak/strong symbol split disappears, and MSVC gets `/std:c++17`. This must
# land before DuckDB configures src/tools/third_party -- this file is included
# from extension_build_tools.cmake (DUCKDB_EXTENSION_CONFIGS) well before those
# add_subdirectory() calls, so it does.
#
# Inherited wholesale from ../quack-oauth, which paid for this knowledge.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to enforce" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)

# The 1.4 LTS build compiles DuckDB's `sqlite3_api_wrapper.cpp` (the stable
# v1.5.3 build does not), which pulls in the Win-SDK headers. At C++17 on
# MSVC, `std::byte` then clashes with the SDK's global `byte`
# (`error C2872: 'byte': ambiguous symbol` in rpcndr.h/wtypes.h). Disable
# std::byte for the LTS line -- DuckDB 1.4.x built fine before C++17 existed
# and we don't use std::byte. Scoped to v1.4.x so the stable build is untouched.
set(_gdrive_ddb_ver "$ENV{DUCKDB_GIT_VERSION}")
if(NOT _gdrive_ddb_ver)
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/duckdb" describe --tags
    OUTPUT_VARIABLE _gdrive_ddb_ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(_gdrive_ddb_ver MATCHES "^v?1\\.4\\.")
  add_compile_definitions(_HAS_STD_BYTE=0)
  message(STATUS "gdrive: DuckDB ${_gdrive_ddb_ver} (1.4 LTS) -- defining _HAS_STD_BYTE=0")
endif()

# ---------------------------------------------------------------------------
# Extension version, computed here rather than left to DuckDB.
#
# duckdb_extension_generate_version() runs
#
#     git describe --tags --always --match '${VERSIONING_TAG_MATCH}'
#
# and those single quotes are LITERAL: CMake passes `'v*.*.*'` to git, quotes
# included, which matches no tag. `--always` then falls back to the short
# commit hash, so the extension version is the hash on every build, tagged or
# not. Verified against this repo at v0.1.0:
#
#     --match "'v*.*.*'"  ->  40c9822      (what DuckDB computes)
#     --match  'v*.*.*'   ->  v0.1.0       (what it meant to compute)
#
# docs/RELEASE.md used to promise that tagging produces v0.1.0 on the
# artifact. It does not, and did not; the promise was never checked because
# check_extension_stamp.sh verifies the DUCKDB version field, not this one.
#
# So compute it correctly and pass it explicitly. Not hardcoded: a literal
# "v0.1.0" here would silently ship a stale version the first time someone
# tags without remembering to edit this file.
execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --always --match v*.*.*
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GDRIVE_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GDRIVE_GIT_DESCRIBE)
    find_package(Git QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --match v*.*.*
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE GDRIVE_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
message(STATUS "gdrive extension version: ${GDRIVE_GIT_DESCRIBE}")

# Extension from this repo.
duckdb_extension_load(gdrive
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    EXTENSION_VERSION "${GDRIVE_GIT_DESCRIBE}"
)

# Needed by the live SQL tests: `COPY ... TO 'gdrive://...' (FORMAT parquet)`
# and read_parquet over the fixture files.
duckdb_extension_load(parquet)
