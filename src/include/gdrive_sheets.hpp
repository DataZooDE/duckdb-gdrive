#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Google Sheets logic behind read_sheet_range() and COPY ... (FORMAT
// sheet_range). PURE: no duckdb.hpp, no I/O, never throws across its API --
// so the Catch2 binary compiles it directly (D-1: nothing to mock).
//
// Everything that talks to sheets.googleapis.com is in gdrive_sheets_functions
// .cpp; this file holds what can be decided from text alone: A1 addresses,
// where a write lands and what OVERWRITE clears, how a large write is split,
// how a value becomes a Sheets CellData, and how a grid-data response becomes
// typed columns. See D-15 in CLAUDE.md for why writes go through the Sheets
// API rather than a CSV re-upload.
// ---------------------------------------------------------------------------

//! Sheets' own limits: 10 million cells per spreadsheet (all tabs together),
//! 18,278 columns (ZZZ).
constexpr int64_t kSheetsMaxCells = 10000000;
constexpr int64_t kSheetsMaxColumns = 18278;
//! Bytes of JSON per batchUpdate request. Sheets documents no exact limit but
//! recommends ~2 MB payloads; a write above this is split (not atomic then).
constexpr size_t kSheetsRequestBudgetBytes = 2 * 1024 * 1024;
//! A new tab's grid when the data is smaller: what the Sheets UI creates.
constexpr int64_t kSheetsDefaultRows = 1000;
constexpr int64_t kSheetsDefaultColumns = 26;

//! An A1 address: 'Tab', 'Tab!B2', 'Tab!A1:F200', 'Tab!A1:F' (open rows),
//! 'Tab!A:F' (whole columns), 'Tab!2:5' (whole rows), "'My Tab'!A1". Rows and
//! columns are 1-based. 0 in first_* means "a bare tab" (no cells named); 0 in
//! last_* means open-ended in that dimension.
struct SheetRange {
	std::string sheet;
	int64_t first_row = 0;
	int64_t first_col = 0;
	int64_t last_row = 0;
	int64_t last_col = 0;
	//! A ':' was given: last_* are bounds the user chose, not just the anchor.
	bool has_end = false;

	bool HasCells() const {
		return first_row > 0 && first_col > 0;
	}
};

struct SheetRangeParse {
	bool ok = false;
	std::string error;
	SheetRange range;
};

//! "A" -> 1, "Z" -> 26, "AA" -> 27, "ZZZ" -> 18278. 0 if not a column (or
//! past ZZZ). Case-insensitive.
int64_t SheetColumnIndex(const std::string &letters);
//! 1 -> "A", 27 -> "AA". Empty for < 1.
std::string SheetColumnLetters(int64_t index);

//! Parse an A1 address. The tab is required: a range without one ("A1:B2")
//! is an error, since a COPY that wrote into "whichever tab is first" would
//! be a guess. A single cell has has_end false and last_* == first_*.
SheetRangeParse ParseSheetRange(const std::string &text);

//! The tab name as Sheets wants it in A1 notation: always single-quoted, an
//! embedded quote doubled. Always quoting is valid A1 and never ambiguous
//! (a tab named "A1" or "Q3 plan" needs it anyway).
std::string QuoteSheetName(const std::string &sheet);
//! "'Tab'!A1:F200", "'Tab'!A1:F", "'Tab'!A:F", "'Tab'!B2", "'Tab'".
std::string FormatSheetRange(const SheetRange &range);

//! A block of cells, 0-based, as the Sheets GridRange counts. end_* are
//! exclusive; -1 means unbounded (to the edge of the grid).
struct SheetGridRange {
	int64_t start_row = 0;
	int64_t start_col = 0;
	int64_t end_row = -1;
	int64_t end_col = -1;
};

//! Where a write of `rows` x `cols` anchored at `target` goes: the target's
//! top-left cell (A1 for a bare tab) down and right. When the target gives
//! explicit bounds ('Tab!A1:C' or 'Tab!A1:C10') the block must fit them.
//! False, with `error`, when it does not fit, or runs past Sheets' column
//! limit or the cell limit.
bool SheetWriteBlock(const SheetRange &target, int64_t rows, int64_t cols, SheetGridRange &block, std::string &error);

//! What OVERWRITE clears before writing: from the target's top-left cell down
//! and right, to the target's explicit bounds when it has them, else to the
//! edge of the grid. Only cell VALUES (and formulas) are cleared; formats stay.
SheetGridRange SheetClearRegion(const SheetRange &target);

//! The grid a write needs: at least the current size, grown to hold `block`.
void SheetGridFor(int64_t grid_rows, int64_t grid_cols, const SheetGridRange &block, int64_t &rows, int64_t &cols);

//! False, with `error`, when a tab of `rows` x `cols` beside `other_cells` in
//! the other tabs would pass the 10-million-cell limit.
bool CheckSheetCellLimit(int64_t other_cells, int64_t rows, int64_t cols, std::string &error);

//! Split items of the given serialised sizes into consecutive runs whose sizes
//! sum to at most `budget` (a single larger item gets a run of its own).
//! Returns [begin, end) index pairs covering everything, in order.
std::vector<std::pair<size_t, size_t>> SplitByByteBudget(const std::vector<size_t> &sizes, size_t budget);

// --- Writing values -----------------------------------------------------------

//! How a value is written. DATE / DATE_TIME / TIME are serial numbers plus a
//! number format, so they read back as dates -- in Sheets and here.
enum class SheetValueKind { EMPTY, NUMBER, BOOLEAN, TEXT, DATE, DATE_TIME, TIME };

struct SheetWriteValue {
	SheetValueKind kind = SheetValueKind::EMPTY;
	double number = 0;
	bool boolean = false;
	std::string text;
	//! DATE / DATE_TIME: days since 1970-01-01; DATE_TIME / TIME: microseconds
	//! into the day.
	int64_t days = 0;
	int64_t micros = 0;
};

//! Sheets' serial date: day 0 is 1899-12-30 (no 1900 leap-year bug, unlike
//! Excel), the fraction is the time of day.
double SheetSerialFromEpoch(int64_t days_since_epoch, int64_t micros_of_day);
//! The inverse; microseconds rounded to the millisecond (Sheets keeps no more).
void SheetSerialToEpoch(double serial, int64_t &days_since_epoch, int64_t &micros_of_day);

//! A JSON string literal, quotes included. UTF-8 passes through.
std::string JsonQuote(const std::string &text);

//! One CellData for an updateCells request with fields "userEnteredValue":
//! {"userEnteredValue":{"numberValue":1.5}}, {"userEnteredValue":
//! {"stringValue":"=1+1"}} -- a stringValue is never parsed, so a formula-
//! looking string stays text (RAW semantics). EMPTY and non-finite numbers are
//! {} (the cell is cleared).
std::string SheetCellJson(const SheetWriteValue &value);
//! The same value for values.append with valueInputOption=RAW: a number, a
//! boolean, a string, or "" for EMPTY.
std::string SheetRawValueJson(const SheetWriteValue &value);
//! {"type":"DATE","pattern":"yyyy-mm-dd"} for the date kinds, else empty.
std::string SheetNumberFormatJson(SheetValueKind kind);

// --- batchUpdate requests -----------------------------------------------------

//! updateCells writing `rows` (each already `{"values":[CellData,...]}`)
//! from 0-based (row, col).
std::string SheetUpdateCellsRequest(int64_t sheet_id, int64_t row, int64_t col, const std::vector<std::string> &rows,
                                    size_t begin, size_t end);
//! updateCells clearing the values of `region` (formats untouched).
std::string SheetClearValuesRequest(int64_t sheet_id, const SheetGridRange &region);
//! repeatCell giving one column's rows [row_begin, row_end) a number format.
std::string SheetNumberFormatRequest(int64_t sheet_id, int64_t row_begin, int64_t row_end, int64_t col,
                                     SheetValueKind kind);
//! addSheet with a chosen sheetId, so later requests in the SAME batch can
//! address it -- a new tab and its data are then one atomic batchUpdate.
std::string SheetAddSheetRequest(int64_t sheet_id, const std::string &title, int64_t rows, int64_t cols);
//! updateSheetProperties setting the grid to rows x cols. Idempotent, unlike
//! appendDimension: a retried request cannot grow the grid twice.
std::string SheetResizeGridRequest(int64_t sheet_id, int64_t rows, int64_t cols);
//! {"requests":[...]}.
std::string SheetBatchUpdateBody(const std::vector<std::string> &requests);

// --- Reading responses --------------------------------------------------------

struct SheetProperties {
	int64_t sheet_id = 0;
	std::string title;
	int64_t rows = 0;
	int64_t cols = 0;
};

//! spreadsheets.get with fields=sheets.properties(sheetId,title,gridProperties).
bool ParseSheetProperties(const std::string &json, std::vector<SheetProperties> &out, std::string &error);

//! A sheetId not in `taken`, starting from `candidate` (any non-negative
//! 31-bit value the caller drew at random).
int64_t PickSheetId(const std::vector<SheetProperties> &taken, int64_t candidate);

//! values.append's response: the range it wrote (updates.updatedRange).
bool ParseAppendedRange(const std::string &json, SheetRange &out, std::string &error);

//! One cell of a grid-data read: its effectiveValue and the TYPE of its
//! effective number format (Sheets says DATE/TIME/DATE_TIME outright -- no
//! format-string parsing as for Excel).
enum class SheetCellKind { EMPTY, NUMBER, STRING, BOOLEAN, ERROR_CELL };
enum class SheetFormatKind { OTHER, DATE, DATE_TIME, TIME };

struct SheetCell {
	SheetCellKind kind = SheetCellKind::EMPTY;
	double number = 0;
	bool boolean = false;
	//! STRING text, or an ERROR's code ("#DIV/0!").
	std::string text;
	SheetFormatKind format = SheetFormatKind::OTHER;
};

//! spreadsheets.get?ranges=...&fields=sheets(data(rowData(values(
//! effectiveValue,effectiveFormat.numberFormat.type)))): the cells, row-major,
//! padded to `width` columns (0: the widest row), trailing all-empty rows
//! dropped.
bool ParseSheetGrid(const std::string &json, int64_t width, std::vector<std::vector<SheetCell>> &rows,
                    std::string &error);

//! The DuckDB type a column becomes. The same rules as duckdb-sharepoint's
//! read_excel_range: only booleans -> BOOLEAN; only numbers -> by format (all
//! DATE -> DATE, DATE and DATE_TIME -> TIMESTAMP, all TIME -> TIME, all plain
//! and integral -> BIGINT, else DOUBLE; dates mixed with plain numbers ->
//! DOUBLE); anything else, or nothing -> VARCHAR. Error cells do not vote.
enum class SheetColumnType { VARCHAR, DOUBLE, BIGINT, BOOLEAN, DATE, TIMESTAMP, TIME };
SheetColumnType InferSheetColumnType(const std::vector<const SheetCell *> &cells);

//! A cell's text in a VARCHAR column: strings as-is, booleans TRUE/FALSE,
//! dates as ISO text, integral numbers without ".0", others with up to 15
//! significant digits, errors as their code.
std::string SheetCellText(const SheetCell &cell);

//! Column names from a header row: trimmed text; empty -> column<N>
//! (1-based); duplicates (case-insensitive) get _2, _3.
std::vector<std::string> SheetHeaderNames(const std::vector<SheetCell> &header, size_t width);

} // namespace gdrive
} // namespace duckdb
