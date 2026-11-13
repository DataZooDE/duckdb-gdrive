// read_sheet_range() and COPY ... TO '<Sheet>' (FORMAT sheet_range): a native
// Google Sheet read and written by tab and A1 range through the Sheets API v4,
// in place -- other tabs, formulas outside the range and formatting stay as
// they are. The sibling duckdb-sharepoint's read_excel_range / FORMAT
// excel_range over the Graph workbook API, kept parallel in names, options and
// column behaviour. Why the Sheets API and not a CSV re-upload: D-15.
//
// The cell logic is pure (gdrive_sheets.*, Catch2); everything here talks to
// sheets.googleapis.com and is live-tested (D-1).
#include "gdrive_sheets_functions.hpp"
#include "gdrive_banner.hpp"
#include "gdrive_copy_rewrite.hpp"
#include "gdrive_filesystem.hpp"
#include "gdrive_glob.hpp"
#include "gdrive_internal.hpp"
#include "gdrive_sheets.hpp"
#include "gdrive_table_util.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <cmath>
#include <cstdint>
#include <random>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace gdrive {

namespace {

constexpr const char *kSheetsHost = "sheets.googleapis.com";
constexpr const char *kSpreadsheetMime = "application/vnd.google-apps.spreadsheet";

//! The spreadsheet an argument names, and what is needed to call Sheets for it.
struct Spreadsheet {
	GDriveAuthContext auth;
	std::string id;
	std::string path; // as the user wrote it, for messages
};

std::string What(const Spreadsheet &s) {
	return "Sheet '" + s.path + "'";
}

//! Resolve a gdrive:// path, id: path or browser address to a native Sheet.
//! Anything else is refused before any Sheets call: a CSV is read with
//! read_csv, and writing a "range" into one would mean rewriting its bytes.
Spreadsheet OpenSpreadsheet(ClientContext &context, const std::string &arg, const char *function) {
	Spreadsheet s;
	s.path = PathArgument(Value(arg), function);
	auto parsed = ParseGDriveUri(s.path);
	if (!parsed.ok) {
		throw InvalidInputException("gdrive: %s", parsed.error);
	}
	if (!parsed.uri.version.empty()) {
		throw InvalidInputException("gdrive: %s: the Sheets API reads and writes the current Sheet only, not an "
		                            "earlier version: '%s'",
		                            function, s.path);
	}
	if (parsed.uri.kind != GDriveUriKind::FILE_ID && HasGlobMetacharacters(s.path)) {
		throw InvalidInputException("gdrive: %s takes one Sheet, not a glob: '%s'", function, s.path);
	}
	auto opener = ClientData::Get(context).file_opener.get();
	auto &fs = GDriveFileSystem::ForDatabase(DatabaseInstance::GetDatabase(context));
	auto items = fs.ResolveItems(s.path, opener, /*rich=*/false, /*folder_as_item=*/true);
	if (items.size() != 1) {
		throw InvalidInputException("gdrive: no such Sheet: '%s'", s.path);
	}
	const auto &meta = items[0].meta;
	if (meta.mime_type != kSpreadsheetMime) {
		throw InvalidInputException("gdrive: %s works on a native Google Sheet, and '%s' is %s%s", function, s.path,
		                            meta.IsFolder() ? "a folder" : "not one (" + meta.mime_type + ")",
		                            meta.IsFolder() ? "" : "; read a file with read_csv / read_parquet");
	}
	s.auth = GetAuthContext(opener, s.path);
	s.id = meta.id;
	return s;
}

std::string SpreadsheetPath(const Spreadsheet &s) {
	return "/v4/spreadsheets/" + QueryEncode(s.id);
}

//! One Sheets API call; any failure is thrown as the classified error.
std::string SheetsCall(GDriveClient &client, const Spreadsheet &s, const std::string &method, const std::string &path,
                       const std::string &body, int64_t DriveCallStats::*counter, bool retry_ambiguous) {
	auto resp = client.HostRequest(kSheetsHost, method, path, body, counter, retry_ambiguous);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, What(s));
	}
	return resp.body;
}

std::vector<SheetProperties> GetTabs(GDriveClient &client, const Spreadsheet &s) {
	const auto body =
	    SheetsCall(client, s, "GET",
	               SpreadsheetPath(s) + "?fields=" +
	                   QueryEncode("sheets.properties(sheetId,title,gridProperties(rowCount,columnCount))"),
	               "", &DriveCallStats::sheets_get, false);
	std::vector<SheetProperties> tabs;
	std::string error;
	if (!ParseSheetProperties(body, tabs, error)) {
		throw IOException("gdrive: unexpected response reading the tabs of %s: %s", What(s), error);
	}
	return tabs;
}

const SheetProperties *FindTab(const std::vector<SheetProperties> &tabs, const std::string &title) {
	for (auto &t : tabs) {
		if (t.title == title) {
			return &t;
		}
	}
	return nullptr;
}

SheetRange ParseRangeOrThrow(const std::string &text) {
	auto parsed = ParseSheetRange(text);
	if (!parsed.ok) {
		throw InvalidInputException("gdrive: %s", parsed.error);
	}
	return parsed.range;
}

// ===========================================================================
// read_sheet_range(path, 'Tab!A1:F' [, header := true] [, all_varchar := false])
// ===========================================================================

struct SheetReadBindData : public TableFunctionData {
	vector<SheetColumnType> types;
	std::vector<std::vector<SheetCell>> rows;
};

LogicalType ToLogicalType(SheetColumnType t) {
	switch (t) {
	case SheetColumnType::DOUBLE:
		return LogicalType::DOUBLE;
	case SheetColumnType::BIGINT:
		return LogicalType::BIGINT;
	case SheetColumnType::BOOLEAN:
		return LogicalType::BOOLEAN;
	case SheetColumnType::DATE:
		return LogicalType::DATE;
	case SheetColumnType::TIMESTAMP:
		return LogicalType::TIMESTAMP;
	case SheetColumnType::TIME:
		return LogicalType::TIME;
	default:
		return LogicalType::VARCHAR;
	}
}

Value ToValue(const SheetCell &cell, SheetColumnType t) {
	const auto type = ToLogicalType(t);
	if (cell.kind == SheetCellKind::EMPTY) {
		return Value(type);
	}
	if (t == SheetColumnType::VARCHAR) {
		return Value(SheetCellText(cell));
	}
	if (t == SheetColumnType::BOOLEAN) {
		return cell.kind == SheetCellKind::BOOLEAN ? Value::BOOLEAN(cell.boolean) : Value(type);
	}
	if (cell.kind != SheetCellKind::NUMBER) {
		return Value(type); // an error cell (#DIV/0!) in a typed column
	}
	int64_t days, micros;
	switch (t) {
	case SheetColumnType::BIGINT:
		return Value::BIGINT(static_cast<int64_t>(cell.number));
	case SheetColumnType::DOUBLE:
		return Value::DOUBLE(cell.number);
	case SheetColumnType::DATE:
		SheetSerialToEpoch(cell.number, days, micros);
		return Value::DATE(date_t(static_cast<int32_t>(days)));
	case SheetColumnType::TIMESTAMP:
		SheetSerialToEpoch(cell.number, days, micros);
		return Value::TIMESTAMP(timestamp_t(days * Interval::MICROS_PER_DAY + micros));
	case SheetColumnType::TIME:
		SheetSerialToEpoch(cell.number, days, micros);
		return Value::TIME(dtime_t(micros));
	default:
		return Value(type);
	}
}

bool BoolOption(TableFunctionBindInput &input, const char *name, bool fallback) {
	auto value = duckdb_compat::FindNamedParameter(input.named_parameters, name);
	return (value && !value->IsNull()) ? BooleanValue::Get(*value) : fallback;
}

unique_ptr<FunctionData> RangeBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		if (input.inputs[1].IsNull()) {
			throw InvalidInputException("read_sheet_range: the range must not be NULL");
		}
		const auto range = ParseRangeOrThrow(StringValue::Get(input.inputs[1]));
		const bool header = BoolOption(input, "header", true);
		const bool all_varchar = BoolOption(input, "all_varchar", false);
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("read_sheet_range: the path must not be NULL");
		}
		auto sheet = OpenSpreadsheet(context, StringValue::Get(input.inputs[0]), "read_sheet_range");
		auto client = CreateGDriveClient(sheet.auth);
		// One spreadsheets.get with grid data: effectiveValue (a formula's
		// result) plus the number format's TYPE, which says DATE / TIME /
		// DATE_TIME outright. values.get would lose the dates.
		const std::string fields = "sheets(data(rowData(values(effectiveValue,effectiveFormat/numberFormat/type))))";
		auto resp = client->HostRequest(kSheetsHost, "GET",
		                                SpreadsheetPath(sheet) + "?includeGridData=true&ranges=" +
		                                    QueryEncode(FormatSheetRange(range)) + "&fields=" + QueryEncode(fields),
		                                "", &DriveCallStats::sheets_get, false);
		if (!resp.ok) {
			// Sheets answers a tab that does not exist with 400 "Unable to parse
			// range" -- say which tab.
			if (resp.error.kind == GDriveErrorKind::INVALID_REQUEST &&
			    resp.error.message.find("Unable to parse range") != std::string::npos) {
				throw InvalidInputException("gdrive: %s has no tab '%s' (or '%s' is not a range in it)", What(sheet),
				                            range.sheet, FormatSheetRange(range));
			}
			ThrowGDriveError(resp.error, What(sheet));
		}
		int64_t width = 0;
		if (range.HasCells() && range.last_col > 0) {
			width = range.last_col - range.first_col + 1;
		}
		std::vector<std::vector<SheetCell>> rows;
		std::string error;
		if (!ParseSheetGrid(resp.body, width, rows, error)) {
			throw IOException("gdrive: unexpected response reading %s: %s", What(sheet), error);
		}
		const size_t cols = rows.empty() ? static_cast<size_t>(std::max<int64_t>(width, 1)) : rows.front().size();
		std::vector<SheetCell> header_cells;
		if (header && !rows.empty()) {
			header_cells = std::move(rows.front());
			rows.erase(rows.begin());
		}
		auto data = make_uniq<SheetReadBindData>();
		auto column_names = SheetHeaderNames(header_cells, std::max<size_t>(cols, 1));
		for (size_t c = 0; c < column_names.size(); c++) {
			std::vector<const SheetCell *> column;
			column.reserve(rows.size());
			for (auto &row : rows) {
				if (c < row.size()) {
					column.push_back(&row[c]);
				}
			}
			const auto t = all_varchar ? SheetColumnType::VARCHAR : InferSheetColumnType(column);
			data->types.push_back(t);
			return_types.push_back(ToLogicalType(t));
			names.emplace_back(column_names[c]);
		}
		for (auto &row : rows) {
			row.resize(column_names.size());
		}
		data->rows = std::move(rows);
		result = std::move(data);
	});
	return result;
}

struct CursorState : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

unique_ptr<GlobalTableFunctionState> CursorInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CursorState>();
}

void RangeScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<SheetReadBindData>();
	auto &state = input.global_state->Cast<CursorState>();
	idx_t out = 0;
	while (state.cursor < data.rows.size() && out < STANDARD_VECTOR_SIZE) {
		auto &row = data.rows[state.cursor];
		for (idx_t c = 0; c < data.types.size(); c++) {
			output.SetValue(c, out, ToValue(row[c], data.types[c]));
		}
		state.cursor++;
		out++;
	}
	duckdb_compat::SetChildCardinality(output, out);
}

// ===========================================================================
// COPY ... TO '<Sheet>' (FORMAT sheet_range, RANGE 'Tab!A1' [, HEADER b]
//                                          [, OVERWRITE | APPEND])
//
// The public format only rewrites the statement to the internal one with
// USE_TMP_FILE false (gdrive_copy_rewrite.hpp): a staged tmp_<name> moved over
// the target would trash the Sheet. Rows are buffered in the sink and written
// in finalize, so a COPY whose query fails mid-way writes nothing at all.
// ===========================================================================

constexpr const char *kRangeFormat = "sheet_range";
constexpr const char *kRangeFormatInternal = "__gdrive_sheet_range";

enum class WriteMode { BLOCK, OVERWRITE, APPEND };

bool IsNested(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::UNION:
		return true;
	default:
		return false;
	}
}

SheetValueKind WriteKindFor(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return SheetValueKind::BOOLEAN;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		// A Sheets number is a double: DECIMAL and HUGEINT lose digits past
		// ~15-17 significant ones, as they would typed into the UI.
		return SheetValueKind::NUMBER;
	case LogicalTypeId::DATE:
		return SheetValueKind::DATE;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
		return SheetValueKind::DATE_TIME;
	case LogicalTypeId::TIME:
		return SheetValueKind::TIME;
	default:
		return SheetValueKind::TEXT; // VARCHAR, and anything else as its text
	}
}

SheetWriteValue ToWriteValue(const Value &v, SheetValueKind kind) {
	SheetWriteValue out;
	if (v.IsNull()) {
		return out;
	}
	switch (kind) {
	case SheetValueKind::BOOLEAN:
		out.kind = kind;
		out.boolean = BooleanValue::Get(v);
		return out;
	case SheetValueKind::NUMBER:
		out.kind = kind;
		out.number = v.GetValue<double>();
		return out;
	case SheetValueKind::DATE: {
		const auto date = v.GetValue<date_t>();
		if (!Value::IsFinite(date)) {
			return out;
		}
		out.kind = kind;
		out.days = date.days;
		return out;
	}
	case SheetValueKind::DATE_TIME: {
		const auto ts = v.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
		if (!Value::IsFinite(ts)) {
			return out;
		}
		const int64_t micros = Timestamp::GetEpochMicroSeconds(ts);
		int64_t days = micros / Interval::MICROS_PER_DAY;
		int64_t rest = micros % Interval::MICROS_PER_DAY;
		if (rest < 0) {
			rest += Interval::MICROS_PER_DAY;
			days--;
		}
		out.kind = kind;
		out.days = days;
		out.micros = rest;
		return out;
	}
	case SheetValueKind::TIME: {
		// Time::Convert, not dtime_t's member: `micros` before 2.0, `value` in 2.0.
		int32_t hour, minute, second, micros;
		Time::Convert(v.GetValue<dtime_t>(), hour, minute, second, micros);
		out.kind = kind;
		out.micros = ((hour * 60LL + minute) * 60 + second) * 1000000 + micros;
		return out;
	}
	default:
		out.kind = SheetValueKind::TEXT;
		out.text = v.ToString();
		if (out.text.empty()) {
			out.kind = SheetValueKind::EMPTY;
		}
		return out;
	}
}

string CopyStringOption(const CopyInfo &info, const char *name) {
	auto v = FindCopyOption(info.options, name);
	if (!v || v->size() != 1 || (*v)[0].IsNull()) {
		return string();
	}
	return (*v)[0].ToString();
}

struct SheetWriteBindData : public FunctionData {
	SheetRange target;
	bool header = true;
	//! HEADER was given at all (APPEND writes the names only when asked, or
	//! when it creates the tab).
	bool header_given = false;
	WriteMode mode = WriteMode::BLOCK;
	vector<string> names;
	vector<SheetValueKind> kinds;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<SheetWriteBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<SheetWriteBindData>();
		return FormatSheetRange(target) == FormatSheetRange(other.target) && names == other.names &&
		       mode == other.mode && header == other.header;
	}
};

unique_ptr<FunctionData> SheetWriteBind(ClientContext &, CopyFunctionBindInput &input,
                                        const duckdb_compat::CopyBindNames &names,
                                        const vector<LogicalType> &sql_types) {
	auto &info = input.info;
	// Only reachable through the rewrite, which pins USE_TMP_FILE false and
	// leaves this marker. A direct FORMAT __gdrive_sheet_range could otherwise
	// stage a tmp file and move it over the Sheet.
	if (!IsRewrittenInPlace(info)) {
		throw InvalidInputException("gdrive: write Sheets with FORMAT %s", kRangeFormat);
	}
	auto data = make_uniq<SheetWriteBindData>();
	const auto range = CopyStringOption(info, "range");
	if (range.empty()) {
		throw InvalidInputException("gdrive: FORMAT sheet_range needs RANGE 'Tab!A1' (the tab, and where to write)");
	}
	data->target = ParseRangeOrThrow(range);
	if (auto header = FindCopyOption(info.options, "header")) {
		data->header_given = true;
		data->header = CopyFlagSet(header);
	}
	if (FindCopyOption(info.options, kOverwriteMarker)) {
		data->mode = WriteMode::OVERWRITE;
	} else if (FindCopyOption(info.options, kAppendMarker)) {
		data->mode = WriteMode::APPEND;
	}
	for (idx_t i = 0; i < sql_types.size(); i++) {
		const auto name = duckdb_compat::Str(names[i]);
		if (IsNested(sql_types[i])) {
			throw InvalidInputException("gdrive: column \"%s\" is %s; a Sheet cell holds one value -- convert it first "
			                            "(e.g. to_json(\"%s\") or array_to_string(\"%s\", ','))",
			                            name, sql_types[i].ToString(), name, name);
		}
		data->names.push_back(name);
		data->kinds.push_back(WriteKindFor(sql_types[i]));
	}
	return std::move(data);
}

struct SheetWriteGlobalState : public GlobalFunctionData {
	Spreadsheet sheet;
	std::vector<SheetProperties> tabs;
	//! The target tab, or nullptr when this COPY creates it.
	const SheetProperties *tab = nullptr;
	int64_t other_cells = 0;
	//! BLOCK / OVERWRITE: one `{"values":[CellData...]}` per row. APPEND: one
	//! `[raw, raw, ...]` per row. Header first when it is written.
	vector<string> rows;
	bool header_row = false;
	int64_t data_rows = 0;
};

unique_ptr<GlobalFunctionData> SheetWriteInitGlobal(ClientContext &context, FunctionData &bind_p,
                                                    const string &file_path) {
	auto &bind = bind_p.Cast<SheetWriteBindData>();
	auto g = make_uniq<SheetWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		g->sheet = OpenSpreadsheet(context, file_path, "FORMAT sheet_range");
		auto client = CreateGDriveClient(g->sheet.auth);
		g->tabs = GetTabs(*client, g->sheet);
		g->tab = FindTab(g->tabs, bind.target.sheet);
		for (auto &t : g->tabs) {
			if (&t != g->tab) {
				g->other_cells += t.rows * t.cols;
			}
		}
		g->header_row = bind.mode == WriteMode::APPEND ? (bind.header_given ? bind.header : !g->tab) : bind.header;
		if (g->header_row) {
			std::string row = bind.mode == WriteMode::APPEND ? "[" : "{\"values\":[";
			for (size_t c = 0; c < bind.names.size(); c++) {
				SheetWriteValue v;
				v.kind = SheetValueKind::TEXT;
				v.text = bind.names[c];
				if (c) {
					row += ',';
				}
				row += bind.mode == WriteMode::APPEND ? SheetRawValueJson(v) : SheetCellJson(v);
			}
			row += bind.mode == WriteMode::APPEND ? "]" : "]}";
			g->rows.push_back(std::move(row));
		}
	});
	return std::move(g);
}

unique_ptr<LocalFunctionData> SheetWriteInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

void SheetWriteSink(ExecutionContext &, FunctionData &bind_p, GlobalFunctionData &gstate_p, LocalFunctionData &,
                    DataChunk &input) {
	auto &bind = bind_p.Cast<SheetWriteBindData>();
	auto &g = gstate_p.Cast<SheetWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		const bool raw = bind.mode == WriteMode::APPEND;
		const int64_t cols = static_cast<int64_t>(bind.names.size());
		for (idx_t r = 0; r < input.size(); r++) {
			std::string row = raw ? "[" : "{\"values\":[";
			for (idx_t c = 0; c < input.ColumnCount(); c++) {
				const auto v = ToWriteValue(input.GetValue(c, r), bind.kinds[c]);
				if (c) {
					row += ',';
				}
				row += raw ? SheetRawValueJson(v) : SheetCellJson(v);
			}
			row += raw ? "]" : "]}";
			g.rows.push_back(std::move(row));
			g.data_rows++;
		}
		// Fail while sinking, not after buffering a hundred million cells: no
		// spreadsheet can hold more than 10M, whatever the anchor.
		std::string error;
		if (!CheckSheetCellLimit(g.other_cells, static_cast<int64_t>(g.rows.size()), cols, error)) {
			throw InvalidInputException("gdrive: cannot write to %s: %s", What(g.sheet), error);
		}
	});
}

void SheetWriteCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

std::vector<size_t> RowSizes(const vector<string> &rows) {
	std::vector<size_t> sizes;
	sizes.reserve(rows.size());
	for (auto &r : rows) {
		sizes.push_back(r.size() + 1);
	}
	return sizes;
}

int64_t NewSheetId(const std::vector<SheetProperties> &tabs) {
	std::random_device rd;
	return PickSheetId(tabs, static_cast<int64_t>(rd() & 0x7ffffffe) + 1);
}

std::string PartialWriteMessage(const Spreadsheet &s, int64_t done, int64_t total, const std::string &cause) {
	return StringUtil::Format("gdrive: %s was partially written: %lld of %lld rows landed before a request failed "
	                          "(a write larger than one request is split and is not atomic). Re-run the COPY with "
	                          "OVERWRITE to replace the range as a whole. Cause: %s",
	                          What(s), static_cast<long long>(std::max<int64_t>(done, 0)), static_cast<long long>(total), cause);
}

//! BLOCK and OVERWRITE: everything a write needs -- a new tab, a larger grid,
//! the OVERWRITE clear, the cells and the date formats -- as batchUpdate
//! requests, ONE call when it fits the byte budget (atomic: Sheets applies a
//! batch all or nothing), else one per chunk with the structure in the first.
void WriteBlock(GDriveClient &client, const SheetWriteBindData &bind, SheetWriteGlobalState &g) {
	const bool overwrite = bind.mode == WriteMode::OVERWRITE;
	const int64_t cols = static_cast<int64_t>(bind.names.size());
	const int64_t rows = static_cast<int64_t>(g.rows.size());
	std::vector<std::string> structure;
	int64_t sheet_id;
	const bool creating = g.tab == nullptr;
	SheetGridRange block;
	if (rows > 0) {
		std::string error;
		if (!SheetWriteBlock(bind.target, rows, cols, block, error)) {
			throw InvalidInputException("gdrive: cannot write to %s: %s", What(g.sheet), error);
		}
	} else {
		block.start_row = bind.target.HasCells() ? bind.target.first_row - 1 : 0;
		block.start_col = bind.target.HasCells() ? bind.target.first_col - 1 : 0;
		block.end_row = block.start_row;
		block.end_col = block.start_col;
	}
	int64_t grid_rows, grid_cols;
	if (creating) {
		sheet_id = NewSheetId(g.tabs);
		SheetGridFor(kSheetsDefaultRows, kSheetsDefaultColumns, block, grid_rows, grid_cols);
		structure.push_back(SheetAddSheetRequest(sheet_id, bind.target.sheet, grid_rows, grid_cols));
	} else {
		sheet_id = g.tab->sheet_id;
		SheetGridFor(g.tab->rows, g.tab->cols, block, grid_rows, grid_cols);
		if (grid_rows != g.tab->rows || grid_cols != g.tab->cols) {
			structure.push_back(SheetResizeGridRequest(sheet_id, grid_rows, grid_cols));
		}
	}
	std::string error;
	if (!CheckSheetCellLimit(g.other_cells, grid_rows, grid_cols, error)) {
		throw InvalidInputException("gdrive: cannot write to %s: %s", What(g.sheet), error);
	}
	if (overwrite && !creating) {
		structure.push_back(SheetClearValuesRequest(sheet_id, SheetClearRegion(bind.target)));
	}

	auto chunks = SplitByByteBudget(RowSizes(g.rows), kSheetsRequestBudgetBytes);
	if (chunks.empty()) {
		chunks.emplace_back(0, 0); // structure only (a new tab, a clear)
	}
	if (structure.empty() && rows == 0) {
		return; // nothing to do
	}
	const int64_t first_data = g.header_row ? 1 : 0;
	int64_t written = 0;
	for (size_t k = 0; k < chunks.size(); k++) {
		std::vector<std::string> requests;
		if (k == 0) {
			requests = structure;
		}
		const auto begin = chunks[k].first, end = chunks[k].second;
		if (end > begin) {
			requests.push_back(SheetUpdateCellsRequest(sheet_id, block.start_row + static_cast<int64_t>(begin),
			                                           block.start_col, g.rows, begin, end));
			// Dates are serial numbers; give their cells a date format so they
			// read back as dates. Data rows only: the header keeps its format.
			const int64_t fmt_begin = std::max<int64_t>(static_cast<int64_t>(begin), first_data);
			if (fmt_begin < static_cast<int64_t>(end)) {
				for (size_t c = 0; c < bind.kinds.size(); c++) {
					if (!SheetNumberFormatJson(bind.kinds[c]).empty()) {
						requests.push_back(SheetNumberFormatRequest(
						    sheet_id, block.start_row + fmt_begin, block.start_row + static_cast<int64_t>(end),
						    block.start_col + static_cast<int64_t>(c), bind.kinds[c]));
					}
				}
			}
		}
		// Only an OVERWRITE is repeatable whatever happened to the first try:
		// it sets the whole range. A new tab is not (a second addSheet fails),
		// so its first batch is never retried.
		const bool retry = overwrite && !(creating && k == 0);
		auto resp = client.HostRequest(kSheetsHost, "POST", SpreadsheetPath(g.sheet) + ":batchUpdate",
		                               SheetBatchUpdateBody(requests), &DriveCallStats::sheets_update, retry);
		if (!resp.ok) {
			if (k == 0) {
				ThrowGDriveError(resp.error, What(g.sheet));
			}
			throw IOException(PartialWriteMessage(g.sheet, written - first_data, rows - first_data,
			                                      FormatUserMessage(resp.error, What(g.sheet))));
		}
		written += static_cast<int64_t>(end - begin);
	}
}

//! APPEND: values.append (RAW) below the data the anchor's table holds, one
//! call per chunk, then the date formats on the rows it reports writing.
void WriteAppend(GDriveClient &client, const SheetWriteBindData &bind, SheetWriteGlobalState &g) {
	int64_t sheet_id;
	if (!g.tab) {
		sheet_id = NewSheetId(g.tabs);
		SheetGridRange block;
		std::string error;
		const int64_t rows = std::max<int64_t>(1, static_cast<int64_t>(g.rows.size()));
		if (!SheetWriteBlock(bind.target, rows, static_cast<int64_t>(bind.names.size()), block, error)) {
			throw InvalidInputException("gdrive: cannot write to %s: %s", What(g.sheet), error);
		}
		int64_t grid_rows, grid_cols;
		SheetGridFor(kSheetsDefaultRows, kSheetsDefaultColumns, block, grid_rows, grid_cols);
		SheetsCall(client, g.sheet, "POST", SpreadsheetPath(g.sheet) + ":batchUpdate",
		           SheetBatchUpdateBody({SheetAddSheetRequest(sheet_id, bind.target.sheet, grid_rows, grid_cols)}),
		           &DriveCallStats::sheets_update, false);
	} else {
		sheet_id = g.tab->sheet_id;
	}
	if (g.rows.empty()) {
		return;
	}
	SheetRange anchor = bind.target;
	if (!anchor.HasCells()) {
		anchor.first_row = anchor.first_col = 1;
		anchor.last_row = anchor.last_col = 1;
		anchor.has_end = false;
	}
	const std::string path = SpreadsheetPath(g.sheet) + "/values/" + QueryEncode(FormatSheetRange(anchor)) +
	                         ":append?valueInputOption=RAW&insertDataOption=OVERWRITE&includeValuesInResponse=false";
	const int64_t first_data = g.header_row ? 1 : 0;
	const int64_t total = static_cast<int64_t>(g.rows.size()) - first_data;
	std::vector<std::string> formats;
	int64_t written = 0;
	for (auto &chunk : SplitByByteBudget(RowSizes(g.rows), kSheetsRequestBudgetBytes)) {
		std::string body = "{\"majorDimension\":\"ROWS\",\"values\":[";
		for (size_t i = chunk.first; i < chunk.second; i++) {
			if (i > chunk.first) {
				body += ',';
			}
			body += g.rows[i];
		}
		body += "]}";
		// Never retried: a repeated append appends twice.
		auto resp = client.HostRequest(kSheetsHost, "POST", path, body, &DriveCallStats::sheets_append, false);
		if (!resp.ok) {
			if (written == 0) {
				ThrowGDriveError(resp.error, What(g.sheet));
			}
			throw IOException(PartialWriteMessage(g.sheet, written - first_data, total,
			                                      FormatUserMessage(resp.error, What(g.sheet))));
		}
		SheetRange landed;
		std::string error;
		if (!ParseAppendedRange(resp.body, landed, error)) {
			throw IOException("gdrive: unexpected response appending to %s: %s", What(g.sheet), error);
		}
		const int64_t header_here = (chunk.first == 0 && g.header_row) ? 1 : 0;
		const int64_t row_begin = landed.first_row - 1 + header_here;
		const int64_t row_end = landed.first_row - 1 + static_cast<int64_t>(chunk.second - chunk.first);
		if (row_begin < row_end) {
			for (size_t c = 0; c < bind.kinds.size(); c++) {
				if (!SheetNumberFormatJson(bind.kinds[c]).empty()) {
					formats.push_back(SheetNumberFormatRequest(sheet_id, row_begin, row_end,
					                                           landed.first_col - 1 + static_cast<int64_t>(c),
					                                           bind.kinds[c]));
				}
			}
		}
		written += static_cast<int64_t>(chunk.second - chunk.first);
	}
	if (!formats.empty()) {
		// Setting a format is repeatable: retried like a read.
		auto resp = client.HostRequest(kSheetsHost, "POST", SpreadsheetPath(g.sheet) + ":batchUpdate",
		                               SheetBatchUpdateBody(formats), &DriveCallStats::sheets_update, true);
		if (!resp.ok) {
			throw IOException("gdrive: the rows were appended to %s, but giving the date columns a date format "
			                  "failed, so they show as serial numbers: %s",
			                  What(g.sheet), FormatUserMessage(resp.error, What(g.sheet)));
		}
	}
}

void SheetWriteFinalize(ClientContext &, FunctionData &bind_p, GlobalFunctionData &gstate_p) {
	auto &bind = bind_p.Cast<SheetWriteBindData>();
	auto &g = gstate_p.Cast<SheetWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		auto client = CreateGDriveClient(g.sheet.auth);
		if (bind.mode == WriteMode::APPEND) {
			WriteAppend(*client, bind, g);
		} else {
			WriteBlock(*client, bind, g);
		}
	});
}

CopyFunctionExecutionMode SheetWriteExecutionMode(bool, bool) {
	// One writer, rows in query order: a range has no "later".
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

void SheetRangeCopyOptions(ClientContext &, CopyOptionsInput &input) {
	AddCopyOption(input.options, kInPlaceMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, kOverwriteMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, kAppendMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, "range", LogicalType::VARCHAR);
	AddCopyOption(input.options, "header", LogicalType::BOOLEAN);
}

BoundStatement SheetRangeCopyPlan(Binder &binder, CopyStatement &stmt) {
	return RewriteCopyInPlace(binder, stmt, kRangeFormatInternal, "Sheet");
}

} // namespace

void RegisterSheetsFunctions(ExtensionLoader &loader) {
	{
		TableFunction fn("read_sheet_range", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RangeScan, RangeBind,
		                 CursorInit);
		duckdb_compat::NameParameters(fn, {"path", "range"});
		duckdb_compat::AddNamedParameter(fn, "header", LogicalType::BOOLEAN);
		duckdb_compat::AddNamedParameter(fn, "all_varchar", LogicalType::BOOLEAN);
		CreateTableFunctionInfo info(fn);
		FunctionDescription desc;
		desc.description =
		    "Read a tab or A1 range of a native Google Sheet, live through the Sheets API: 'Tab!A1:F200', "
		    "'Tab!A1:F' (to the last row), 'Tab!A:F', or a bare tab name; quote a tab with spaces as "
		    "'''My Tab''!A1'. The first row is the header unless header := false. Formula cells read as their "
		    "results; dates, times and booleans keep their types. all_varchar := true reads every column as text. "
		    "The path is a gdrive:// path (the scheme may be left out), gdrive://id:<id>, or the Sheet's browser "
		    "address.";
		desc.examples = {"SELECT * FROM read_sheet_range('gdrive://Finance/sales', 'Forecast!A1:F')",
		                 "SELECT * FROM read_sheet_range('gdrive://Finance/sales', 'Input', header := false)"};
		desc.categories = {"gdrive", "sheets"};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}
	{
		CopyFunction fn(duckdb_compat::FunctionName(kRangeFormat));
		fn.plan = SheetRangeCopyPlan;
		loader.RegisterFunction(fn);
	}
	{
		CopyFunction fn(duckdb_compat::FunctionName(kRangeFormatInternal));
		fn.copy_options = SheetRangeCopyOptions;
		fn.copy_to_bind = SheetWriteBind;
		fn.copy_to_initialize_global = SheetWriteInitGlobal;
		fn.copy_to_initialize_local = SheetWriteInitLocal;
		fn.copy_to_sink = SheetWriteSink;
		fn.copy_to_combine = SheetWriteCombine;
		fn.copy_to_finalize = SheetWriteFinalize;
		fn.execution_mode = SheetWriteExecutionMode;
		loader.RegisterFunction(fn);
	}
}

} // namespace gdrive
} // namespace duckdb
