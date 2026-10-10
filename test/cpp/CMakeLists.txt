# Standalone build of the Catch2 pure-logic tests -- NO DuckDB, NO vcpkg.
#
# The pure sources (URI parsing, glob matching, error classification, the
# service-account / ADC / OAuth parameter logic, version) include no
# duckdb.hpp, so they need only Catch2, two header-only libraries (picojson,
# jwt-cpp) and OpenSSL. Building them here instead of through the full
# extension (`make unit_test`, which configures all of DuckDB and bootstraps
# vcpkg) turns the per-push Catch2 gate from tens of minutes into seconds.
# `make test` still runs the same tests through the in-tree build.
#
# Ported from the sibling duckdb-sharepoint's test/cpp/CMakeLists.txt.
# Fetched sources are pinned by commit, not tag: tags can be moved.
# Catch2 comes from the system if present, else is fetched; picojson and
# jwt-cpp are fetched at the versions vcpkg pins; OpenSSL is the system one
# (libssl-dev on the CI runner).
cmake_minimum_required(VERSION 3.16)
project(gdrive_unit_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG 05e10dfccc28c7f973727c54f850237d07d5e10f # v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)
endif()

find_package(OpenSSL REQUIRED)

# Header-only; populate without running their own CMake.
FetchContent_Declare(picojson GIT_REPOSITORY https://github.com/kazuho/picojson.git GIT_TAG 25fc213cca61ea22b3c2e4db8def9927562ba5f7) # v1.3.0
FetchContent_Declare(jwtcpp GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git GIT_TAG e71e0c2d584baff06925bbb3aad683f677e4d498) # v0.7.1
FetchContent_GetProperties(picojson)
if(NOT picojson_POPULATED)
    FetchContent_Populate(picojson)
endif()
FetchContent_GetProperties(jwtcpp)
if(NOT jwtcpp_POPULATED)
    FetchContent_Populate(jwtcpp)
endif()
# The sources spell it <picojson/picojson.h>, as the vcpkg port installs it.
file(COPY ${picojson_SOURCE_DIR}/picojson.h DESTINATION ${CMAKE_BINARY_DIR}/inc/picojson)

set(GD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

file(GLOB GD_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(gdrive_unit_tests
    ${GD_ROOT}/src/gdrive_version.cpp
    ${GD_ROOT}/src/gdrive_uri.cpp
    ${GD_ROOT}/src/gdrive_errors.cpp
    ${GD_ROOT}/src/gdrive_glob.cpp
    ${GD_ROOT}/src/gdrive_service_account_pure.cpp
    ${GD_ROOT}/src/gdrive_adc.cpp
    ${GD_ROOT}/src/gdrive_oauth_params.cpp
    ${GD_ROOT}/src/gdrive_blockcache.cpp
    ${GD_ROOT}/src/gdrive_identity.cpp
    ${GD_TESTS}
)
# Same ODR-relevant define as the extension build (see the root CMakeLists).
target_compile_definitions(gdrive_unit_tests PRIVATE PICOJSON_USE_INT64)
target_include_directories(gdrive_unit_tests PRIVATE
    ${GD_ROOT}/src/include
    ${CMAKE_BINARY_DIR}/inc
    ${jwtcpp_SOURCE_DIR}/include
)
target_link_libraries(gdrive_unit_tests PRIVATE Catch2::Catch2WithMain OpenSSL::SSL OpenSSL::Crypto)
