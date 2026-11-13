#pragma once

// The in-place COPY rewrite for writers that change a remote object through
// an API instead of writing a file: FORMAT sheet_range
// (gdrive_sheets_functions.cpp). Ported from duckdb-sharepoint's
// sharepoint_copy_rewrite.hpp, which does the same for excel_range.
//
// DuckDB's COPY TO an EXISTING target defaults to USE_TMP_FILE: the writer is
// handed "tmp_<name>" and the operator MoveFile()s it over the target at the
// end. On Drive a move onto a native Sheet TRASHES the Sheet (CLAUDE.md, crew
// review 2026-11-07), so that rename must never happen. The public format
// therefore carries only CopyFunction::plan, which rebinds the statement as an
// internal format with USE_TMP_FILE false and a marker option; the internal
// writer refuses to bind without the marker (the binder consumes USE_TMP_FILE
// itself before copy_to_bind). Same hook and option flow on DuckDB 1.5.6 and
// 2.0 (whose options are keyed by Identifier).

#include "duckdb.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/planner/binder.hpp"

#include <cstdint>
#include <type_traits>

namespace duckdb {
namespace gdrive {

//! Declared by every internal writer, set only by RewriteCopyInPlace.
constexpr const char *kInPlaceMarker = "__gdrive_in_place";
//! COPY's own OVERWRITE and APPEND, carried past the binder (which would
//! consume them as file options) to the writer, where they mean "clear the
//! range first" and "append below the data".
constexpr const char *kOverwriteMarker = "__gdrive_overwrite";
constexpr const char *kAppendMarker = "__gdrive_append";

//! A COPY option's values, or nullptr; the map's key is a string before 2.0
//! and an Identifier in 2.0.
template <class MAP>
const vector<Value> *FindCopyOption(const MAP &options, const string &name) {
	auto entry = options.find(typename MAP::key_type(name));
	return entry == options.end() ? nullptr : &entry->second;
}

template <class MAP>
void AddCopyOption(MAP &options, const char *name, const LogicalType &type) {
	options[typename MAP::key_type(name)] = CopyOption(type, CopyOptionMode::WRITE_ONLY);
}

//! True if the COPY came through RewriteCopyInPlace.
inline bool IsRewrittenInPlace(const CopyInfo &info) {
	return FindCopyOption(info.options, kInPlaceMarker) != nullptr;
}

//! A bare option (`OVERWRITE`) or one set to a true value.
inline bool CopyFlagSet(const vector<Value> *values) {
	return values && (values->empty() || (*values)[0].DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>());
}

//! Rebind `stmt` as `internal_format`, in place (USE_TMP_FILE false). `what`
//! names the target in errors. OVERWRITE and APPEND become markers (never
//! reaching DuckDB's file handling); both at once, or OVERWRITE_OR_IGNORE,
//! are refused.
inline BoundStatement RewriteCopyInPlace(Binder &binder, CopyStatement &stmt, const char *internal_format,
                                         const char *what) {
	auto copy = stmt.Copy();
	auto &info = *copy->Cast<CopyStatement>().info;
	if (CopyFlagSet(FindCopyOption(info.options, "use_tmp_file"))) {
		throw InvalidInputException("gdrive: USE_TMP_FILE is not supported for %s writes: they change the %s in place",
		                            what, what);
	}
	info.format = internal_format;
	using Key = typename std::decay<decltype(info.options)>::type::key_type;
	if (FindCopyOption(info.options, "overwrite_or_ignore")) {
		throw InvalidInputException("gdrive: OVERWRITE_OR_IGNORE is a file option, not supported for %s writes", what);
	}
	const bool replace = CopyFlagSet(FindCopyOption(info.options, "overwrite"));
	const bool append = CopyFlagSet(FindCopyOption(info.options, "append"));
	if (replace && append) {
		throw InvalidInputException("gdrive: OVERWRITE and APPEND exclude each other for %s writes", what);
	}
	info.options.erase(Key("overwrite"));
	info.options.erase(Key("append"));
	if (replace) {
		info.options[Key(kOverwriteMarker)] = {Value::BOOLEAN(true)};
	}
	if (append) {
		info.options[Key(kAppendMarker)] = {Value::BOOLEAN(true)};
	}
	info.options[Key("use_tmp_file")] = {Value::BOOLEAN(false)};
	info.options[Key(kInPlaceMarker)] = {Value::BOOLEAN(true)};
	return binder.Bind(*copy);
}

} // namespace gdrive
} // namespace duckdb
