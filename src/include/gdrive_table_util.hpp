#pragma once

// Shared plumbing for the metadata table functions (read_gdrive_files,
// search_gdrive, gdrive_drives, gdrive_permissions, gdrive_versions,
// gdrive_changes). Each one reads everything it returns at BIND time -- the
// answers are small and the column set depends on nothing at scan time -- and
// then emits the rows from a cursor. The same shape as duckdb-sharepoint's.

#include "gdrive_rows.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace gdrive {

struct RowsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

struct RowsCursorState : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

inline unique_ptr<GlobalTableFunctionState> RowsCursorInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<RowsCursorState>();
}

inline void RowsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<RowsBindData>();
	auto &state = input.global_state->Cast<RowsCursorState>();
	idx_t out = 0;
	while (state.cursor < data.rows.size() && out < STANDARD_VECTOR_SIZE) {
		auto &row = data.rows[state.cursor];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, out, row[c]);
		}
		state.cursor++;
		out++;
	}
	duckdb_compat::SetChildCardinality(output, out);
}

//! Empty text is NULL: Drive omits a field rather than sending "".
inline Value TextOrNull(const std::string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

//! An RFC 3339 timestamp from Drive as a UTC TIMESTAMP, NULL when absent.
inline Value TimestampOrNull(const std::string &rfc3339) {
	int64_t micros;
	if (rfc3339.empty() || !ParseRfc3339Micros(rfc3339, micros)) {
		return Value(LogicalType::TIMESTAMP);
	}
	return Value::TIMESTAMP(timestamp_t(micros));
}

inline Value SizeOrNull(int64_t size) {
	return size < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(size);
}

//! A table function's path argument: NULL is refused, and a path without the
//! scheme is taken relative to the secret's root, as the sibling extension's
//! functions do.
inline std::string PathArgument(const Value &value, const char *function) {
	if (value.IsNull()) {
		throw InvalidInputException("%s: the path must not be NULL", function);
	}
	auto arg = StringValue::Get(value);
	if (arg.rfind("gdrive://", 0) != 0) {
		arg = "gdrive://" + arg;
	}
	return arg;
}

//! A nextPageToken chain longer than this is a server bug, not a listing.
constexpr int kMaxPages = 10000;

} // namespace gdrive
} // namespace duckdb
