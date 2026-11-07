#pragma once

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! gdrive_permissions().
void RegisterPermissionFunctions(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
