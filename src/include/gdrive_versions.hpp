#pragma once

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! gdrive_versions().
void RegisterVersionFunctions(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
