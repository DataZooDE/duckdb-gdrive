cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME gdrive)

find_package(OpenSSL REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# ---------------------------------------------------------------------------
# picojson stores parsed numbers as int64_t only when PICOJSON_USE_INT64 is
# defined BEFORE picojson.h is first included in a translation unit; otherwise
# every number is a double.
#
# That is an ODR trap, not a preference. jwt-cpp's
# `traits/kazuho-picojson/defaults.h` defines it, so any TU including jwt-cpp
# sees one set of picojson inline functions while a TU including
# <picojson/picojson.h> directly sees a different set. Linking both into one
# binary is undefined behaviour whose observable symptom -- whether a parsed
# number is retrievable as int64_t or double -- depends on link order.
#
# Defining it for the whole project makes every TU agree. Applied here rather
# than per-file so a future #include cannot silently reintroduce the split.
add_compile_definitions(PICOJSON_USE_INT64)

# DuckDB ships cpp-httplib (namespace `duckdb_httplib_openssl`) under its
# third_party tree. Use it directly rather than adding a separate vcpkg dep --
# same choice as ../erpl-web and ../quack-oauth. Pure-logic targets (the
# Catch2 binary) deliberately do NOT get this include path: if a "pure"
# source ever needs it, that source is not pure and belongs in DUCKDB_SOURCES.
include_directories(./duckdb/third_party/httplib)

# Anonymous usage telemetry, shared with ../erpl, ../erpl-web, ../quack-oauth.
# Native only -- it uses duckdb_httplib_openssl::SSLClient, not portable to
# wasm. Opt-out: `gdrive_telemetry_enabled` setting / DATAZOO_DISABLE_TELEMETRY.
if(NOT EMSCRIPTEN)
    include_directories(posthog-telemetry/include)
    add_subdirectory(posthog-telemetry)
endif()

# ---------------------------------------------------------------------------
# datazoo-oauth2 (D-11, REQ-A-01/A-02; design in docs/hld.md section 5.4).
#
# The shared OAuth2 stack: authorization-code + PKCE, the loopback redirect
# server, cross-platform browser launch, and get-token-with-refresh against a
# DuckDB secret. Extracted from erpl-web, which now consumes it too, so there
# is one implementation rather than a fork.
#
# Pinned by commit, not tracked by branch: each consumer pins its own, so
# neither is forced to upgrade on the other's schedule (R-9).
#
# Native only, like posthog-telemetry: the loopback server and the browser
# launcher have no meaning under emscripten.
#
# NOT wired into the extension's sources yet -- PROVIDER authorization_code is
# the next slice. This pins and builds the dependency so that landing it is a
# code change rather than a build-system change.
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
    add_subdirectory(datazoo-oauth2)
endif()

# ---------------------------------------------------------------------------
# Pure-logic sources: no `#include "duckdb.hpp"`, no I/O. Compiled into BOTH
# the extension and the Catch2 unit-test binary. See docs/implementation-plan
# .md section 1 -- these are the only things Catch2 is allowed to test, and
# because they do no I/O there is nothing to mock.
# ---------------------------------------------------------------------------
#
# GLOBbed rather than listed. CMakeLists.txt is the highest-collision file in
# a DuckDB extension, and several tracks add pure sources concurrently; a
# glob keeps it out of everyone's file set. CONFIGURE_DEPENDS makes CMake
# re-glob when a file appears, so `make unit_test` picks up a new source
# without a manual reconfigure.
#
# The pattern is deliberately narrow -- only files that are genuinely pure
# match. A source needing DuckDB goes in DUCKDB_SOURCES below and is listed
# explicitly, because that list is small and rarely contested.
file(GLOB PURE_SOURCES CONFIGURE_DEPENDS
    src/gdrive_version.cpp
    src/gdrive_uri.cpp
    src/gdrive_errors.cpp
    src/gdrive_glob.cpp
    src/gdrive_service_account_pure.cpp
    src/gdrive_adc.cpp
    src/gdrive_oauth_params.cpp
)

# ---------------------------------------------------------------------------
# DuckDB-coupled sources. Everything here is exercised by live SQL tests
# against real Google Drive, never by a fake.
# ---------------------------------------------------------------------------
file(GLOB DUCKDB_SOURCES CONFIGURE_DEPENDS
    src/gdrive_extension.cpp
    src/gdrive_client.cpp
    src/gdrive_secret.cpp
    src/gdrive_auth.cpp
    src/gdrive_service_account_duckdb.cpp
    src/gdrive_filesystem.cpp
    src/gdrive_path_cache.cpp
    src/gdrive_file_handle.cpp
    src/gdrive_export.cpp
    src/gdrive_upload.cpp
    src/gdrive_mutate.cpp
    src/gdrive_stats.cpp
    src/gdrive_vfs_functions.cpp
    # Not PURE despite having no DuckDB dependency: it opens a file and writes
    # to it, and the pure contract forbids I/O.
    src/gdrive_trace.cpp
)

set(EXTENSION_SOURCES ${PURE_SOURCES} ${DUCKDB_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# DuckDB's extension build macros default the target to C++11; bump to match
# the project standard. Sibling-extension pattern (../erpl-web, ../quack-oauth).
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)

if(NOT EMSCRIPTEN)
    target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp posthog_telemetry datazoo_oauth2)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp posthog_telemetry datazoo_oauth2)
else()
    target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# ---------------------------------------------------------------------------
# Catch2 unit-test binary -- pure logic only, no DuckDB linkage.
#
# Skipped under EMSCRIPTEN: the binary is not runnable in wasm, and on the
# wasm_threads variant linking an *executable* against the vcpkg OpenSSL fails
# because that port was not built with the atomics/bulk-memory features
# --shared-memory requires. The extension itself is a shared library and links
# fine. (Lesson inherited from ../quack-oauth.)
#
# Skipped on MSVC by default for a related reason. On the LTS Windows job's
# `x64-windows-static-md-*-vs2019comp` triplet, vcpkg builds Catch2 against
# the DYNAMIC CRT while DuckDB compiles our sources against the STATIC one:
#
#   Catch2.lib(catch_stringref.cpp.obj): error LNK2038: mismatch detected for
#   'RuntimeLibrary': value 'MD_DynamicRelease' doesn't match value
#   'MT_StaticRelease' in gdrive_errors.cpp.obj
#
# The EXTENSION links cleanly there; only this test executable does not. And
# it is a test binary, not a shipped artifact -- failing a release build over
# it is backwards. What it covers is pure logic (URI parsing, glob matching,
# error classification, JWT claim construction) with no platform-dependent
# behaviour whatsoever, so running it on Linux and macOS is real coverage,
# not a compromise.
#
# Opt back in with -DGDRIVE_BUILD_UNIT_TESTS=ON if you are working on the
# pure layer from Windows and have a matching Catch2.
# ---------------------------------------------------------------------------
if(MSVC)
  set(GDRIVE_BUILD_UNIT_TESTS_DEFAULT OFF)
else()
  set(GDRIVE_BUILD_UNIT_TESTS_DEFAULT ON)
endif()
option(GDRIVE_BUILD_UNIT_TESTS "Build the Catch2 pure-logic test binary"
       ${GDRIVE_BUILD_UNIT_TESTS_DEFAULT})

find_package(Catch2 3 CONFIG QUIET)
if(Catch2_FOUND AND NOT EMSCRIPTEN AND GDRIVE_BUILD_UNIT_TESTS)
  file(GLOB GDRIVE_UNIT_TEST_SOURCES CONFIGURE_DEPENDS test/cpp/test_*.cpp)
  if(GDRIVE_UNIT_TEST_SOURCES)
    add_executable(gdrive_unit_tests
      ${PURE_SOURCES}
      ${GDRIVE_UNIT_TEST_SOURCES}
    )
    target_include_directories(gdrive_unit_tests PRIVATE src/include)
    target_compile_features(gdrive_unit_tests PRIVATE cxx_std_17)
    target_link_libraries(gdrive_unit_tests PRIVATE
      Catch2::Catch2WithMain
      OpenSSL::SSL
      OpenSSL::Crypto
      jwt-cpp::jwt-cpp
    )
    set_target_properties(gdrive_unit_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
    )
    # Match the static CRT (/MT) of the vcpkg deps, which ship from a
    # *-static-* triplet. DuckDB's stable build sets MSVC_RUNTIME_LIBRARY
    # globally and this naked add_executable inherits it; the 1.4 LTS build
    # does not, so the target would default to /MD and fail with
    # `LNK2038: RuntimeLibrary mismatch`. Same naked-target gotcha as the
    # cxx_std_17 feature above.
    if(MSVC)
      set_target_properties(gdrive_unit_tests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
      )
    endif()
  endif()
endif()
