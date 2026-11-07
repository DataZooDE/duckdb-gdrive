#pragma once

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! read_gdrive_files(), search_gdrive() and gdrive_drives().
void RegisterFileFunctions(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
