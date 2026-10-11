#pragma once

#include <string>

namespace duckdb {
namespace gdrive {

// The extension's own version string, independent of DuckDB's. Pure logic so
// the Catch2 binary can assert on it without linking DuckDB.
std::string GdriveVersion();

//! The same string as a C string with static storage, for the load banner
//! (datazoo::BannerInfo holds `const char *`).
const char *GdriveVersionCString();

} // namespace gdrive
} // namespace duckdb
