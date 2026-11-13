#pragma once

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! read_sheet_range() and COPY ... TO '<a Google Sheet>' (FORMAT sheet_range):
//! a native Google Sheet read and written by tab and A1 range through the
//! Sheets API v4 (sheets.googleapis.com). See D-15 in CLAUDE.md.
void RegisterSheetsFunctions(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
