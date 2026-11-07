#pragma once

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! gdrive_changes().
void RegisterChangeFunctions(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
