#pragma once

#include <string>

namespace duckdb {
namespace gdrive {

// The extension's own version string, independent of DuckDB's. Pure logic so
// the Catch2 binary can assert on it without linking DuckDB.
std::string GdriveVersion();

} // namespace gdrive
} // namespace duckdb
