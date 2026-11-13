// Google Sheets logic for read_sheet_range() and COPY ... (FORMAT sheet_range).
// PURE: no duckdb.hpp, no I/O (see gdrive_sheets.hpp). The cell-typing rules
// are duckdb-sharepoint's read_excel_range rules, kept parallel on purpose.
#include "gdrive_sheets.hpp"

#include <picojson/picojson.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>

namespace duckdb {
namespace gdrive {

namespace {

//! Sheets has no row limit of its own beyond the cell limit; a row number
//! past it can never hold a cell.
constexpr int64_t kSheetsMaxRows = kSheetsMaxCells;
//! Sheets' day 0 (1899-12-30) is this many days before 1970-01-01.
constexpr int64_t kSerialEpochDays = 25569;

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

//! One corner of an A1 range: "$B$3", "B3", "B" (a whole column) or "3" (a
//! whole row). Either part may be missing, not both.
bool ParseCorner(const std::string &text, int64_t &row, int64_t &col) {
	size_t i = 0;
	if (i < text.size() && text[i] == '$') {
		i++;
	}
	const size_t letters_begin = i;
	while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
		i++;
	}
	const std::string letters = text.substr(letters_begin, i - letters_begin);
	if (i < text.size() && text[i] == '$') {
		i++;
	}
	const size_t digits_begin = i;
	while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
		i++;
	}
	const std::string digits = text.substr(digits_begin, i - digits_begin);
	if (i != text.size() || (letters.empty() && digits.empty()) || digits.size() > 9) {
		return false;
	}
	col = 0;
	row = 0;
	if (!letters.empty()) {
		col = SheetColumnIndex(letters);
		if (col == 0) {
			return false;
		}
	}
	if (!digits.empty()) {
		row = std::stoll(digits);
		if (row < 1 || row > kSheetsMaxRows) {
			return false;
		}
	}
	return true;
}

//! "A1", "A1:F200", "A1:F", "A:F", "2:5" into `out`. False if not cells.
bool ParseCells(const std::string &text, SheetRange &out) {
	const auto colon = text.find(':');
	if (colon != std::string::npos && text.find(':', colon + 1) != std::string::npos) {
		return false;
	}
	int64_t r1, c1;
	if (colon == std::string::npos) {
		// A single cell needs both parts; "B" alone is not an address.
		if (!ParseCorner(text, r1, c1) || r1 == 0 || c1 == 0) {
			return false;
		}
		out.first_row = out.last_row = r1;
		out.first_col = out.last_col = c1;
		out.has_end = false;
		return true;
	}
	int64_t r2, c2;
	if (!ParseCorner(text.substr(0, colon), r1, c1) || !ParseCorner(text.substr(colon + 1), r2, c2)) {
		return false;
	}
	// The start corner must name a column unless the range is whole rows
	// ("2:5"), and a row unless it is whole columns ("A:F").
	const bool whole_rows = c1 == 0 && c2 == 0;
	const bool whole_cols = r1 == 0;
	if ((c1 == 0 && !whole_rows) || (whole_rows && (r1 == 0 || r2 == 0))) {
		return false;
	}
	if (whole_cols && (c1 == 0 || c2 == 0 || r2 != 0)) {
		return false;
	}
	out.has_end = true;
	if (whole_rows) {
		out.first_col = 1;
		out.last_col = 0;
		out.first_row = std::min(r1, r2);
		out.last_row = std::max(r1, r2);
		return true;
	}
	out.first_row = whole_cols ? 1 : r1;
	out.first_col = std::min(c1, c2);
	out.last_col = std::max(c1, c2);
	if (r2 == 0) {
		out.last_row = 0; // open rows: 'A1:F'
	} else {
		out.first_row = std::min(r1, r2);
		out.last_row = std::max(r1, r2);
	}
	return true;
}

bool IsIntegral(double v) {
	// 2^53: beyond it a double no longer holds every integer.
	return std::isfinite(v) && std::floor(v) == v && std::fabs(v) <= 9007199254740992.0;
}

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm).
void CivilFromDays(int64_t z, int64_t &y, int &m, int &d) {
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
	m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
	y = yoe + era * 400 + (m <= 2 ? 1 : 0);
}

std::string NumberJson(double v) {
	if (IsIntegral(v) && std::fabs(v) < 1e15) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
		return buf;
	}
	char buf[40];
	// 17 significant digits: a double round-trips exactly.
	std::snprintf(buf, sizeof(buf), "%.17g", v);
	return buf;
}

std::string I64(int64_t v) {
	return std::to_string(v);
}

const picojson::object *Obj(const picojson::value &v) {
	return v.is<picojson::object>() ? &v.get<picojson::object>() : nullptr;
}

const picojson::value *Field(const picojson::object *o, const char *key) {
	if (!o) {
		return nullptr;
	}
	auto it = o->find(key);
	return it == o->end() ? nullptr : &it->second;
}

const picojson::array *ArrayField(const picojson::object *o, const char *key) {
	auto v = Field(o, key);
	return (v && v->is<picojson::array>()) ? &v->get<picojson::array>() : nullptr;
}

int64_t IntField(const picojson::object *o, const char *key, int64_t fallback) {
	auto v = Field(o, key);
	if (v && v->is<int64_t>()) {
		return v->get<int64_t>();
	}
	if (v && v->is<double>()) {
		return static_cast<int64_t>(v->get<double>());
	}
	return fallback;
}

bool Parse(const std::string &json, picojson::value &root, std::string &error) {
	const auto err = picojson::parse(root, json);
	if (!err.empty() || !root.is<picojson::object>()) {
		error = "the response is not a JSON object";
		return false;
	}
	return true;
}

//! errorValue.type -> the code a spreadsheet shows.
std::string ErrorCode(const std::string &type) {
	if (type == "NULL_VALUE") {
		return "#NULL!";
	}
	if (type == "DIVIDE_BY_ZERO") {
		return "#DIV/0!";
	}
	if (type == "VALUE") {
		return "#VALUE!";
	}
	if (type == "REF") {
		return "#REF!";
	}
	if (type == "NAME") {
		return "#NAME?";
	}
	if (type == "NUM") {
		return "#NUM!";
	}
	if (type == "N_A") {
		return "#N/A";
	}
	if (type == "LOADING") {
		return "Loading...";
	}
	return "#ERROR!";
}

SheetCell ToCell(const picojson::value &v) {
	SheetCell cell;
	auto o = Obj(v);
	auto effective_format = Field(o, "effectiveFormat");
	auto number_format = effective_format ? Field(Obj(*effective_format), "numberFormat") : nullptr;
	auto format = number_format ? Obj(*number_format) : nullptr;
	if (auto type = Field(format, "type")) {
		if (type->is<std::string>()) {
			const auto &t = type->get<std::string>();
			cell.format = t == "DATE"        ? SheetFormatKind::DATE
			              : t == "DATE_TIME" ? SheetFormatKind::DATE_TIME
			              : t == "TIME"      ? SheetFormatKind::TIME
			                                 : SheetFormatKind::OTHER;
		}
	}
	auto value_v = Field(o, "effectiveValue");
	auto value = value_v ? Obj(*value_v) : nullptr;
	if (!value) {
		return cell;
	}
	if (auto n = Field(value, "numberValue")) {
		if (n->is<double>() || n->is<int64_t>()) {
			cell.kind = SheetCellKind::NUMBER;
			cell.number = n->is<double>() ? n->get<double>() : static_cast<double>(n->get<int64_t>());
		}
	} else if (auto s = Field(value, "stringValue")) {
		if (s->is<std::string>() && !s->get<std::string>().empty()) {
			cell.kind = SheetCellKind::STRING;
			cell.text = s->get<std::string>();
		}
	} else if (auto b = Field(value, "boolValue")) {
		if (b->is<bool>()) {
			cell.kind = SheetCellKind::BOOLEAN;
			cell.boolean = b->get<bool>();
		}
	} else if (auto e = Field(value, "errorValue")) {
		cell.kind = SheetCellKind::ERROR_CELL;
		auto type = Field(Obj(*e), "type");
		cell.text = ErrorCode(type && type->is<std::string>() ? type->get<std::string>() : std::string());
	}
	return cell;
}

} // namespace

int64_t SheetColumnIndex(const std::string &letters) {
	if (letters.empty() || letters.size() > 3) {
		return 0;
	}
	int64_t index = 0;
	for (unsigned char c : letters) {
		if (!std::isalpha(c)) {
			return 0;
		}
		index = index * 26 + (std::toupper(c) - 'A' + 1);
	}
	return index <= kSheetsMaxColumns ? index : 0;
}

std::string SheetColumnLetters(int64_t index) {
	std::string out;
	while (index > 0) {
		index--;
		out.insert(out.begin(), static_cast<char>('A' + index % 26));
		index /= 26;
	}
	return out;
}

SheetRangeParse ParseSheetRange(const std::string &input) {
	SheetRangeParse result;
	const std::string text = Trim(input);
	if (text.empty()) {
		result.error = "the range is empty; name a tab, e.g. 'Sheet1!A1'";
		return result;
	}
	std::string sheet;
	std::string cells;
	if (text[0] == '\'') {
		// 'Tab name'!A1 -- '' inside the quotes is one quote.
		size_t i = 1;
		bool closed = false;
		while (i < text.size()) {
			if (text[i] == '\'') {
				if (i + 1 < text.size() && text[i + 1] == '\'') {
					sheet += '\'';
					i += 2;
					continue;
				}
				closed = true;
				i++;
				break;
			}
			sheet += text[i++];
		}
		if (!closed) {
			result.error = "unterminated quote in the tab name of '" + text + "'";
			return result;
		}
		if (i < text.size()) {
			if (text[i] != '!') {
				result.error = "expected '!' after the quoted tab name in '" + text + "'";
				return result;
			}
			cells = text.substr(i + 1);
			if (cells.empty()) {
				result.error = "no cells after '!' in '" + text + "'";
				return result;
			}
		}
	} else {
		const auto bang = text.rfind('!');
		if (bang == std::string::npos) {
			SheetRange probe;
			if (ParseCells(text, probe)) {
				result.error = "'" + text + "' names no tab; write it as 'Sheet1!" + text + "'";
				return result;
			}
			sheet = text; // a bare tab name
		} else {
			sheet = text.substr(0, bang);
			cells = text.substr(bang + 1);
			if (cells.empty()) {
				result.error = "no cells after '!' in '" + text + "'";
				return result;
			}
		}
	}
	if (sheet.empty()) {
		result.error = "empty tab name in '" + text + "'";
		return result;
	}
	result.range.sheet = sheet;
	if (!cells.empty() && !ParseCells(cells, result.range)) {
		result.error = "'" + cells + "' is not an A1 range such as A1, A1:F200, A1:F or A:F (columns A..ZZZ)";
		return result;
	}
	result.ok = true;
	return result;
}

std::string QuoteSheetName(const std::string &sheet) {
	std::string out = "'";
	for (char c : sheet) {
		out += c;
		if (c == '\'') {
			out += '\'';
		}
	}
	return out + "'";
}

std::string FormatSheetRange(const SheetRange &range) {
	const std::string tab = QuoteSheetName(range.sheet);
	if (!range.HasCells()) {
		return tab;
	}
	const std::string start = SheetColumnLetters(range.first_col) + std::to_string(range.first_row);
	if (!range.has_end) {
		return tab + "!" + start;
	}
	if (range.last_col == 0) { // whole rows
		return tab + "!" + std::to_string(range.first_row) + ":" + std::to_string(range.last_row);
	}
	const std::string end = SheetColumnLetters(range.last_col) + (range.last_row ? std::to_string(range.last_row) : "");
	return tab + "!" + start + ":" + end;
}

bool SheetWriteBlock(const SheetRange &target, int64_t rows, int64_t cols, SheetGridRange &block, std::string &error) {
	if (rows < 1 || cols < 1) {
		error = "nothing to write";
		return false;
	}
	const int64_t first_row = target.HasCells() ? target.first_row : 1;
	const int64_t first_col = target.HasCells() ? target.first_col : 1;
	block.start_row = first_row - 1;
	block.start_col = first_col - 1;
	block.end_row = block.start_row + rows;
	block.end_col = block.start_col + cols;
	const std::string where = "'" + FormatSheetRange(target) + "'";
	if (target.has_end && target.last_col > 0 && block.end_col > target.last_col) {
		error = std::to_string(cols) + " columns do not fit " + where + " (" +
		        std::to_string(target.last_col - first_col + 1) + " columns); widen the range or name just its top-left "
		        "cell";
		return false;
	}
	if (target.has_end && target.last_row > 0 && block.end_row > target.last_row) {
		error = std::to_string(rows) + " rows do not fit " + where + " (" +
		        std::to_string(target.last_row - first_row + 1) + " rows); widen the range or leave its end row open";
		return false;
	}
	if (block.end_col > kSheetsMaxColumns) {
		error = std::to_string(cols) + " columns from column " + SheetColumnLetters(first_col) +
		        " run past Sheets' last column (ZZZ)";
		return false;
	}
	if (block.end_row > kSheetsMaxRows) {
		error = std::to_string(rows) + " rows from row " + std::to_string(first_row) +
		        " run past the 10,000,000-cell limit of a spreadsheet";
		return false;
	}
	return true;
}

SheetGridRange SheetClearRegion(const SheetRange &target) {
	SheetGridRange region;
	region.start_row = target.HasCells() ? target.first_row - 1 : 0;
	region.start_col = target.HasCells() ? target.first_col - 1 : 0;
	if (target.has_end) {
		region.end_row = target.last_row > 0 ? target.last_row : -1;
		region.end_col = target.last_col > 0 ? target.last_col : -1;
	}
	return region;
}

void SheetGridFor(int64_t grid_rows, int64_t grid_cols, const SheetGridRange &block, int64_t &rows, int64_t &cols) {
	rows = std::max(grid_rows, block.end_row);
	cols = std::max(grid_cols, block.end_col);
}

bool CheckSheetCellLimit(int64_t other_cells, int64_t rows, int64_t cols, std::string &error) {
	// rows * cols stays far from overflow: both are bounded by the parse.
	const int64_t total = other_cells + rows * cols;
	if (total <= kSheetsMaxCells) {
		return true;
	}
	error = "the write needs a grid of " + std::to_string(rows) + " x " + std::to_string(cols) + " cells, which with " +
	        std::to_string(other_cells) + " cells in the other tabs passes Google Sheets' limit of 10,000,000 cells "
	        "per spreadsheet";
	return false;
}

std::vector<std::pair<size_t, size_t>> SplitByByteBudget(const std::vector<size_t> &sizes, size_t budget) {
	std::vector<std::pair<size_t, size_t>> runs;
	size_t begin = 0, used = 0;
	for (size_t i = 0; i < sizes.size(); i++) {
		if (i > begin && used + sizes[i] > budget) {
			runs.emplace_back(begin, i);
			begin = i;
			used = 0;
		}
		used += sizes[i];
	}
	if (begin < sizes.size()) {
		runs.emplace_back(begin, sizes.size());
	}
	return runs;
}

double SheetSerialFromEpoch(int64_t days_since_epoch, int64_t micros_of_day) {
	return static_cast<double>(days_since_epoch + kSerialEpochDays) + static_cast<double>(micros_of_day) / 86400000000.0;
}

void SheetSerialToEpoch(double serial, int64_t &days_since_epoch, int64_t &micros_of_day) {
	constexpr int64_t kMillisPerDay = 86400000;
	double day = std::floor(serial);
	int64_t millis = static_cast<int64_t>(std::llround((serial - day) * kMillisPerDay));
	if (millis >= kMillisPerDay) {
		millis -= kMillisPerDay;
		day += 1;
	}
	days_since_epoch = static_cast<int64_t>(day) - kSerialEpochDays;
	micros_of_day = millis * 1000;
}

std::string JsonQuote(const std::string &text) {
	std::string out;
	out.reserve(text.size() + 2);
	out += '"';
	for (unsigned char c : text) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
	return out;
}

std::string SheetCellJson(const SheetWriteValue &value) {
	switch (value.kind) {
	case SheetValueKind::EMPTY:
		return "{}";
	case SheetValueKind::BOOLEAN:
		return std::string("{\"userEnteredValue\":{\"boolValue\":") + (value.boolean ? "true" : "false") + "}}";
	case SheetValueKind::TEXT:
		return "{\"userEnteredValue\":{\"stringValue\":" + JsonQuote(value.text) + "}}";
	case SheetValueKind::NUMBER:
		if (!std::isfinite(value.number)) {
			return "{}";
		}
		return "{\"userEnteredValue\":{\"numberValue\":" + NumberJson(value.number) + "}}";
	case SheetValueKind::DATE:
		return "{\"userEnteredValue\":{\"numberValue\":" + NumberJson(SheetSerialFromEpoch(value.days, 0)) + "}}";
	case SheetValueKind::DATE_TIME:
		return "{\"userEnteredValue\":{\"numberValue\":" + NumberJson(SheetSerialFromEpoch(value.days, value.micros)) +
		       "}}";
	case SheetValueKind::TIME:
		return "{\"userEnteredValue\":{\"numberValue\":" +
		       NumberJson(static_cast<double>(value.micros) / 86400000000.0) + "}}";
	}
	return "{}";
}

std::string SheetRawValueJson(const SheetWriteValue &value) {
	const std::string cell = SheetCellJson(value);
	switch (value.kind) {
	case SheetValueKind::EMPTY:
		return "\"\"";
	case SheetValueKind::BOOLEAN:
		return value.boolean ? "true" : "false";
	case SheetValueKind::TEXT:
		return JsonQuote(value.text);
	default:
		break;
	}
	// The number inside the CellData: {"userEnteredValue":{"numberValue":N}}.
	const std::string key = "\"numberValue\":";
	const auto at = cell.find(key);
	if (at == std::string::npos) {
		return "\"\""; // a non-finite number: empty
	}
	return cell.substr(at + key.size(), cell.size() - at - key.size() - 2);
}

std::string SheetNumberFormatJson(SheetValueKind kind) {
	switch (kind) {
	case SheetValueKind::DATE:
		return "{\"type\":\"DATE\",\"pattern\":\"yyyy-mm-dd\"}";
	case SheetValueKind::DATE_TIME:
		return "{\"type\":\"DATE_TIME\",\"pattern\":\"yyyy-mm-dd hh:mm:ss\"}";
	case SheetValueKind::TIME:
		return "{\"type\":\"TIME\",\"pattern\":\"hh:mm:ss\"}";
	default:
		return std::string();
	}
}

std::string SheetUpdateCellsRequest(int64_t sheet_id, int64_t row, int64_t col, const std::vector<std::string> &rows,
                                    size_t begin, size_t end) {
	std::string out = "{\"updateCells\":{\"start\":{\"sheetId\":" + I64(sheet_id) + ",\"rowIndex\":" + I64(row) +
	                  ",\"columnIndex\":" + I64(col) + "},\"rows\":[";
	for (size_t i = begin; i < end && i < rows.size(); i++) {
		if (i > begin) {
			out += ',';
		}
		out += rows[i];
	}
	out += "],\"fields\":\"userEnteredValue\"}}";
	return out;
}

namespace {
std::string GridRangeJson(int64_t sheet_id, const SheetGridRange &r) {
	std::string out = "{\"sheetId\":" + I64(sheet_id) + ",\"startRowIndex\":" + I64(r.start_row) +
	                  ",\"startColumnIndex\":" + I64(r.start_col);
	if (r.end_row >= 0) {
		out += ",\"endRowIndex\":" + I64(r.end_row);
	}
	if (r.end_col >= 0) {
		out += ",\"endColumnIndex\":" + I64(r.end_col);
	}
	return out + "}";
}
} // namespace

std::string SheetClearValuesRequest(int64_t sheet_id, const SheetGridRange &region) {
	// updateCells with a range and no rows: every field the mask names is
	// cleared in the range. "userEnteredValue" is the value or formula only.
	return "{\"updateCells\":{\"range\":" + GridRangeJson(sheet_id, region) + ",\"fields\":\"userEnteredValue\"}}";
}

std::string SheetNumberFormatRequest(int64_t sheet_id, int64_t row_begin, int64_t row_end, int64_t col,
                                     SheetValueKind kind) {
	SheetGridRange r;
	r.start_row = row_begin;
	r.end_row = row_end;
	r.start_col = col;
	r.end_col = col + 1;
	return "{\"repeatCell\":{\"range\":" + GridRangeJson(sheet_id, r) +
	       ",\"cell\":{\"userEnteredFormat\":{\"numberFormat\":" + SheetNumberFormatJson(kind) +
	       "}},\"fields\":\"userEnteredFormat.numberFormat\"}}";
}

std::string SheetAddSheetRequest(int64_t sheet_id, const std::string &title, int64_t rows, int64_t cols) {
	return "{\"addSheet\":{\"properties\":{\"sheetId\":" + I64(sheet_id) + ",\"title\":" + JsonQuote(title) +
	       ",\"gridProperties\":{\"rowCount\":" + I64(rows) + ",\"columnCount\":" + I64(cols) + "}}}}";
}

std::string SheetResizeGridRequest(int64_t sheet_id, int64_t rows, int64_t cols) {
	return "{\"updateSheetProperties\":{\"properties\":{\"sheetId\":" + I64(sheet_id) +
	       ",\"gridProperties\":{\"rowCount\":" + I64(rows) + ",\"columnCount\":" + I64(cols) +
	       "}},\"fields\":\"gridProperties.rowCount,gridProperties.columnCount\"}}";
}

std::string SheetSetTabRequest(int64_t sheet_id, const std::string &title, int64_t rows, int64_t cols) {
	return "{\"updateSheetProperties\":{\"properties\":{\"sheetId\":" + I64(sheet_id) + ",\"title\":" +
	       JsonQuote(title) + ",\"gridProperties\":{\"rowCount\":" + I64(rows) + ",\"columnCount\":" + I64(cols) +
	       "}},\"fields\":\"title,gridProperties.rowCount,gridProperties.columnCount\"}}";
}

bool HasDataFileExtension(const std::string &name) {
	static const char *const kExtensions[] = {".csv",   ".tsv",  ".parquet", ".json", ".jsonl",
	                                          ".ndjson", ".xlsx", ".xls",     ".txt"};
	std::string lower(name);
	std::transform(lower.begin(), lower.end(), lower.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (const char *ext : kExtensions) {
		const std::string e(ext);
		if (lower.size() > e.size() && lower.compare(lower.size() - e.size(), e.size(), e) == 0) {
			return true;
		}
	}
	return false;
}

SheetTargetAction DecideSheetTarget(bool found, const std::string &mime_type, SheetTargetForm form,
                                    bool create_allowed, const std::string &name) {
	if (found) {
		return mime_type == "application/vnd.google-apps.spreadsheet" ? SheetTargetAction::OPEN
		                                                               : SheetTargetAction::REFUSE_NOT_SHEET;
	}
	// An id or an address names an object that should exist; there is no
	// folder or name to create one under.
	if (form != SheetTargetForm::PATH || !create_allowed || name.empty()) {
		return SheetTargetAction::NO_SUCH;
	}
	return HasDataFileExtension(name) ? SheetTargetAction::REFUSE_DATA_EXTENSION : SheetTargetAction::CREATE;
}

std::string SheetBatchUpdateBody(const std::vector<std::string> &requests) {
	std::string out = "{\"requests\":[";
	for (size_t i = 0; i < requests.size(); i++) {
		if (i) {
			out += ',';
		}
		out += requests[i];
	}
	return out + "]}";
}

bool ParseSheetProperties(const std::string &json, std::vector<SheetProperties> &out, std::string &error) {
	picojson::value root;
	if (!Parse(json, root, error)) {
		return false;
	}
	auto sheets = ArrayField(Obj(root), "sheets");
	if (!sheets) {
		error = "the response lists no tabs";
		return false;
	}
	for (auto &s : *sheets) {
		auto props_v = Field(Obj(s), "properties");
		auto props = props_v ? Obj(*props_v) : nullptr;
		auto title = Field(props, "title");
		if (!props || !title || !title->is<std::string>()) {
			error = "a tab in the response has no title";
			return false;
		}
		SheetProperties p;
		p.sheet_id = IntField(props, "sheetId", 0); // the first tab's id 0 is omitted
		p.title = title->get<std::string>();
		auto grid_v = Field(props, "gridProperties");
		auto grid = grid_v ? Obj(*grid_v) : nullptr;
		p.rows = IntField(grid, "rowCount", 0);
		p.cols = IntField(grid, "columnCount", 0);
		out.push_back(std::move(p));
	}
	return true;
}

int64_t PickSheetId(const std::vector<SheetProperties> &taken, int64_t candidate) {
	constexpr int64_t kMax = 2147483647;
	int64_t id = candidate < 1 ? 1 : candidate % kMax;
	for (;;) {
		bool used = false;
		for (auto &p : taken) {
			used = used || p.sheet_id == id;
		}
		if (!used) {
			return id;
		}
		id = id % kMax + 1;
	}
}

bool ParseAppendedRange(const std::string &json, SheetRange &out, std::string &error) {
	picojson::value root;
	if (!Parse(json, root, error)) {
		return false;
	}
	auto updates_v = Field(Obj(root), "updates");
	auto range = Field(updates_v ? Obj(*updates_v) : nullptr, "updatedRange");
	if (!range || !range->is<std::string>()) {
		error = "the append response names no updated range";
		return false;
	}
	auto parsed = ParseSheetRange(range->get<std::string>());
	if (!parsed.ok || !parsed.range.HasCells()) {
		error = "unexpected updated range '" + range->get<std::string>() + "'";
		return false;
	}
	out = parsed.range;
	return true;
}

bool ParseSheetGrid(const std::string &json, int64_t width, std::vector<std::vector<SheetCell>> &rows,
                    std::string &error) {
	picojson::value root;
	if (!Parse(json, root, error)) {
		return false;
	}
	auto sheets = ArrayField(Obj(root), "sheets");
	if (!sheets || sheets->empty()) {
		error = "the response holds no tab";
		return false;
	}
	auto data = ArrayField(Obj(sheets->front()), "data");
	rows.clear();
	size_t widest = 0;
	if (data && !data->empty()) {
		if (auto row_data = ArrayField(Obj(data->front()), "rowData")) {
			for (auto &r : *row_data) {
				std::vector<SheetCell> row;
				if (auto values = ArrayField(Obj(r), "values")) {
					for (auto &v : *values) {
						row.push_back(ToCell(v));
					}
				}
				// A row's trailing empty cells carry no width.
				size_t used = row.size();
				while (used > 0 && row[used - 1].kind == SheetCellKind::EMPTY) {
					used--;
				}
				widest = std::max(widest, used);
				rows.push_back(std::move(row));
			}
		}
	}
	// Trailing all-empty rows are not data (an open range reads to the grid's
	// end, a bounded one may name rows nobody filled).
	while (!rows.empty() && std::all_of(rows.back().begin(), rows.back().end(),
	                                    [](const SheetCell &c) { return c.kind == SheetCellKind::EMPTY; })) {
		rows.pop_back();
	}
	const size_t cols = width > 0 ? static_cast<size_t>(width) : widest;
	for (auto &row : rows) {
		row.resize(cols);
	}
	return true;
}

SheetColumnType InferSheetColumnType(const std::vector<const SheetCell *> &cells) {
	bool any_string = false, any_bool = false, any_number = false;
	bool plain = false, date = false, datetime = false, time = false, all_integral = true;
	for (auto *cell : cells) {
		switch (cell->kind) {
		case SheetCellKind::EMPTY:
		case SheetCellKind::ERROR_CELL:
			break;
		case SheetCellKind::STRING:
			any_string = true;
			break;
		case SheetCellKind::BOOLEAN:
			any_bool = true;
			break;
		case SheetCellKind::NUMBER:
			any_number = true;
			switch (cell->format) {
			case SheetFormatKind::OTHER:
				plain = true;
				all_integral = all_integral && IsIntegral(cell->number);
				break;
			case SheetFormatKind::DATE:
				date = true;
				break;
			case SheetFormatKind::DATE_TIME:
				datetime = true;
				break;
			case SheetFormatKind::TIME:
				time = true;
				break;
			}
			break;
		}
	}
	if (any_string || (any_bool && any_number) || (!any_bool && !any_number)) {
		return SheetColumnType::VARCHAR;
	}
	if (any_bool) {
		return SheetColumnType::BOOLEAN;
	}
	if (plain) {
		return (date || datetime || time) ? SheetColumnType::DOUBLE
		       : all_integral             ? SheetColumnType::BIGINT
		                                  : SheetColumnType::DOUBLE;
	}
	if (datetime || (date && time)) {
		return SheetColumnType::TIMESTAMP;
	}
	return date ? SheetColumnType::DATE : SheetColumnType::TIME;
}

std::string SheetCellText(const SheetCell &cell) {
	switch (cell.kind) {
	case SheetCellKind::EMPTY:
		return std::string();
	case SheetCellKind::STRING:
	case SheetCellKind::ERROR_CELL:
		return cell.text;
	case SheetCellKind::BOOLEAN:
		return cell.boolean ? "TRUE" : "FALSE";
	case SheetCellKind::NUMBER:
		break;
	}
	char buf[64];
	if (cell.format != SheetFormatKind::OTHER && std::isfinite(cell.number)) {
		int64_t days, micros;
		SheetSerialToEpoch(cell.number, days, micros);
		const int64_t secs = micros / 1000000;
		char time[16];
		std::snprintf(time, sizeof(time), "%02d:%02d:%02d", static_cast<int>(secs / 3600),
		              static_cast<int>(secs / 60 % 60), static_cast<int>(secs % 60));
		if (cell.format == SheetFormatKind::TIME) {
			return time;
		}
		int64_t y;
		int m, d;
		CivilFromDays(days, y, m, d);
		std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02d", static_cast<long long>(y), m, d);
		return cell.format == SheetFormatKind::DATE ? std::string(buf) : std::string(buf) + " " + time;
	}
	if (IsIntegral(cell.number) && std::fabs(cell.number) < 1e15) {
		std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(cell.number));
	} else {
		std::snprintf(buf, sizeof(buf), "%.15g", cell.number);
	}
	return buf;
}

std::vector<std::string> SheetHeaderNames(const std::vector<SheetCell> &header, size_t width) {
	std::vector<std::string> names;
	std::set<std::string> used; // lower-cased
	for (size_t i = 0; i < width; i++) {
		std::string base = i < header.size() ? Trim(SheetCellText(header[i])) : std::string();
		if (base.empty()) {
			base = "column" + std::to_string(i + 1);
		}
		std::string name = base;
		for (int n = 2; used.count(Lower(name)); n++) {
			name = base + "_" + std::to_string(n);
		}
		used.insert(Lower(name));
		names.push_back(name);
	}
	return names;
}

} // namespace gdrive
} // namespace duckdb
