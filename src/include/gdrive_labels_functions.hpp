#pragma once

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! gdrive_labels().
void RegisterLabelFunctions(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
