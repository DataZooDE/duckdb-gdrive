// Google Sheets pure logic (gdrive_sheets.hpp): A1 addresses, where a write
// lands and what OVERWRITE clears, the byte-budget splitter, CellData
// serialisation (dates as serials), and turning a grid-data response into
// typed columns. Everything that calls sheets.googleapis.com is live-tested
// in test/sql/gdrive_sheet_range*.test.template instead (D-1).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "gdrive_sheets.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace duckdb::gdrive;

namespace {
SheetRange Parsed(const std::string &text) {
	auto p = ParseSheetRange(text);
	INFO(text << ": " << p.error);
	REQUIRE(p.ok);
	return p.range;
}
} // namespace

TEST_CASE("column letters round-trip up to ZZZ", "[sheets]") {
	REQUIRE(SheetColumnIndex("A") == 1);
	REQUIRE(SheetColumnIndex("z") == 26);
	REQUIRE(SheetColumnIndex("AA") == 27);
	REQUIRE(SheetColumnIndex("ZZZ") == 18278);
	REQUIRE(SheetColumnIndex("AAAA") == 0);
	REQUIRE(SheetColumnIndex("A1") == 0);
	REQUIRE(SheetColumnIndex("") == 0);
	for (int64_t i : {1, 26, 27, 52, 702, 703, 18278}) {
		REQUIRE(SheetColumnIndex(SheetColumnLetters(i)) == i);
	}
	REQUIRE(SheetColumnLetters(0).empty());
}

TEST_CASE("A1 forms: cell, block, open rows, whole columns, whole rows, bare tab", "[sheets]") {
	auto cell = Parsed("Forecast!B2");
	REQUIRE(cell.sheet == "Forecast");
	REQUIRE(cell.first_row == 2);
	REQUIRE(cell.first_col == 2);
	REQUIRE_FALSE(cell.has_end);

	auto block = Parsed("Forecast!F200:A1"); // corners in either order
	REQUIRE(block.first_row == 1);
	REQUIRE(block.first_col == 1);
	REQUIRE(block.last_row == 200);
	REQUIRE(block.last_col == 6);
	REQUIRE(block.has_end);

	auto open = Parsed("Forecast!A1:F");
	REQUIRE(open.first_row == 1);
	REQUIRE(open.last_col == 6);
	REQUIRE(open.last_row == 0);

	auto cols = Parsed("Forecast!B:D");
	REQUIRE(cols.first_row == 1);
	REQUIRE(cols.first_col == 2);
	REQUIRE(cols.last_col == 4);
	REQUIRE(cols.last_row == 0);

	auto rows = Parsed("Forecast!2:5");
	REQUIRE(rows.first_col == 1);
	REQUIRE(rows.last_col == 0);
	REQUIRE(rows.first_row == 2);
	REQUIRE(rows.last_row == 5);

	auto bare = Parsed("  Forecast ");
	REQUIRE(bare.sheet == "Forecast");
	REQUIRE_FALSE(bare.HasCells());

	auto abs = Parsed("Forecast!$B$3");
	REQUIRE(abs.first_row == 3);
	REQUIRE(abs.first_col == 2);
}

TEST_CASE("quoted tab names, embedded quotes and bangs", "[sheets]") {
	auto r = Parsed("'My Tab'!A1");
	REQUIRE(r.sheet == "My Tab");
	REQUIRE(Parsed("'It''s'!A1:B2").sheet == "It's");
	REQUIRE(Parsed("'Q3!plan'!C4").sheet == "Q3!plan");
	REQUIRE(Parsed("'2026'").sheet == "2026");
	// Unquoted with a space: the last '!' splits.
	REQUIRE(Parsed("My Tab!A1").sheet == "My Tab");
}

TEST_CASE("malformed addresses are refused with a reason", "[sheets]") {
	for (const char *bad : {"", "A1:B2", "A1", "Tab!", "'Tab", "'Tab'A1", "Tab!A0", "Tab!AAAA1", "Tab!A1:B2:C3",
	                        "Tab!1A", "''!A1", "Tab!A", "Tab!A:5"}) {
		auto p = ParseSheetRange(bad);
		INFO(bad);
		REQUIRE_FALSE(p.ok);
		REQUIRE_FALSE(p.error.empty());
	}
	REQUIRE(ParseSheetRange("A1:B2").error.find("Sheet1!A1:B2") != std::string::npos);
}

TEST_CASE("formatting always quotes the tab", "[sheets]") {
	REQUIRE(FormatSheetRange(Parsed("Forecast!A1:F200")) == "'Forecast'!A1:F200");
	REQUIRE(FormatSheetRange(Parsed("Forecast!A1:F")) == "'Forecast'!A1:F");
	REQUIRE(FormatSheetRange(Parsed("Forecast!A:F")) == "'Forecast'!A1:F");
	REQUIRE(FormatSheetRange(Parsed("Forecast!2:5")) == "'Forecast'!2:5");
	REQUIRE(FormatSheetRange(Parsed("'It''s'!B2")) == "'It''s'!B2");
	REQUIRE(FormatSheetRange(Parsed("Forecast")) == "'Forecast'");
	REQUIRE(QuoteSheetName("A1") == "'A1'");
	// Round trip.
	REQUIRE(Parsed(FormatSheetRange(Parsed("'It''s'!C3:D9"))).sheet == "It's");
}

TEST_CASE("a write block starts at the anchor and must fit explicit bounds", "[sheets]") {
	SheetGridRange b;
	std::string error;
	REQUIRE(SheetWriteBlock(Parsed("T!B2"), 3, 4, b, error));
	REQUIRE(b.start_row == 1);
	REQUIRE(b.start_col == 1);
	REQUIRE(b.end_row == 4);
	REQUIRE(b.end_col == 5);

	REQUIRE(SheetWriteBlock(Parsed("T"), 1, 1, b, error));
	REQUIRE(b.start_row == 0);
	REQUIRE(b.start_col == 0);

	REQUIRE(SheetWriteBlock(Parsed("T!A1:C"), 1000, 3, b, error));
	REQUIRE_FALSE(SheetWriteBlock(Parsed("T!A1:C"), 2, 4, b, error));
	REQUIRE(error.find("4 columns do not fit") != std::string::npos);
	REQUIRE_FALSE(SheetWriteBlock(Parsed("T!A1:C2"), 3, 3, b, error));
	REQUIRE(error.find("3 rows do not fit") != std::string::npos);

	REQUIRE_FALSE(SheetWriteBlock(Parsed("T!ZZZ1"), 1, 2, b, error));
	REQUIRE(error.find("ZZZ") != std::string::npos);
	REQUIRE_FALSE(SheetWriteBlock(Parsed("T!A1"), 0, 2, b, error));
}

TEST_CASE("OVERWRITE clears from the anchor to the grid edge or the given bounds", "[sheets]") {
	auto open = SheetClearRegion(Parsed("T!B3"));
	REQUIRE(open.start_row == 2);
	REQUIRE(open.start_col == 1);
	REQUIRE(open.end_row == -1);
	REQUIRE(open.end_col == -1);

	auto bare = SheetClearRegion(Parsed("T"));
	REQUIRE(bare.start_row == 0);
	REQUIRE(bare.end_col == -1);

	auto cols = SheetClearRegion(Parsed("T!A1:C"));
	REQUIRE(cols.end_col == 3);
	REQUIRE(cols.end_row == -1);

	auto block = SheetClearRegion(Parsed("T!B2:D10"));
	REQUIRE(block.end_row == 10);
	REQUIRE(block.end_col == 4);
}

TEST_CASE("grid growth and the 10M-cell limit", "[sheets]") {
	SheetGridRange b;
	b.end_row = 5000;
	b.end_col = 3;
	int64_t rows, cols;
	SheetGridFor(1000, 26, b, rows, cols);
	REQUIRE(rows == 5000);
	REQUIRE(cols == 26);

	std::string error;
	REQUIRE(CheckSheetCellLimit(0, 1000000, 10, error));
	REQUIRE_FALSE(CheckSheetCellLimit(1, 1000000, 10, error));
	REQUIRE(error.find("10,000,000") != std::string::npos);
}

TEST_CASE("the byte-budget splitter keeps order and never splits an item", "[sheets]") {
	REQUIRE(SplitByByteBudget({}, 10).empty());
	auto runs = SplitByByteBudget({4, 4, 4, 4}, 10);
	REQUIRE(runs.size() == 2);
	REQUIRE(runs[0] == std::make_pair<size_t, size_t>(0, 2));
	REQUIRE(runs[1] == std::make_pair<size_t, size_t>(2, 4));
	// An oversized item gets a run of its own.
	runs = SplitByByteBudget({3, 50, 3}, 10);
	REQUIRE(runs.size() == 3);
	REQUIRE(runs[1] == std::make_pair<size_t, size_t>(1, 2));
	// Exactly at the budget fits.
	REQUIRE(SplitByByteBudget({5, 5}, 10).size() == 1);
}

TEST_CASE("Sheets serial dates: day 0 is 1899-12-30, no 1900 leap bug", "[sheets]") {
	REQUIRE(SheetSerialFromEpoch(0, 0) == 25569.0);         // 1970-01-01
	REQUIRE(SheetSerialFromEpoch(-25569, 0) == 0.0);        // 1899-12-30
	REQUIRE(SheetSerialFromEpoch(-25509, 0) == 60.0);       // 1900-02-28 (Excel would say 59)
	REQUIRE(SheetSerialFromEpoch(20454, 43200000000) == 46023.5); // 2026-01-01 12:00
	int64_t days, micros;
	SheetSerialToEpoch(46023.5, days, micros);
	REQUIRE(days == 20454);
	REQUIRE(micros == 43200000000);
	SheetSerialToEpoch(0.75, days, micros); // a time alone
	REQUIRE(days == -25569);
	REQUIRE(micros == 64800000000);
	// Rounding to the millisecond that tips into the next day.
	SheetSerialToEpoch(1.9999999999, days, micros);
	REQUIRE(days == -25567);
	REQUIRE(micros == 0);
}

TEST_CASE("CellData serialisation: typed values, RAW strings, empty cells", "[sheets]") {
	SheetWriteValue v;
	REQUIRE(SheetCellJson(v) == "{}");

	v.kind = SheetValueKind::NUMBER;
	v.number = 42;
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"numberValue":42}})");
	v.number = 0.1;
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"numberValue":0.10000000000000001}})");
	v.number = 1.0 / 0.0;
	REQUIRE(SheetCellJson(v) == "{}");

	v.kind = SheetValueKind::BOOLEAN;
	v.boolean = true;
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"boolValue":true}})");

	// A formula-looking string stays a string: stringValue is never parsed.
	v.kind = SheetValueKind::TEXT;
	v.text = "=1+1";
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"stringValue":"=1+1"}})");
	v.text = "a\"b\\c\nd\x01";
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"stringValue":"a\"b\\c\nd\u0001"}})");
	v.text = "Grüße";
	REQUIRE(SheetCellJson(v) == "{\"userEnteredValue\":{\"stringValue\":\"Grüße\"}}");

	v.kind = SheetValueKind::DATE;
	v.days = 20454; // 2026-01-01
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"numberValue":46023}})");
	v.kind = SheetValueKind::DATE_TIME;
	v.micros = 43200000000;
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"numberValue":46023.5}})");
	v.kind = SheetValueKind::TIME;
	v.micros = 21600000000; // 06:00
	REQUIRE(SheetCellJson(v) == R"({"userEnteredValue":{"numberValue":0.25}})");
}

TEST_CASE("RAW values for values.append mirror the CellData", "[sheets]") {
	SheetWriteValue v;
	REQUIRE(SheetRawValueJson(v) == "\"\"");
	v.kind = SheetValueKind::NUMBER;
	v.number = 2.5;
	REQUIRE(SheetRawValueJson(v) == "2.5");
	v.number = 0.0 / 0.0;
	REQUIRE(SheetRawValueJson(v) == "\"\"");
	v.kind = SheetValueKind::TEXT;
	v.text = "=A1";
	REQUIRE(SheetRawValueJson(v) == "\"=A1\"");
	v.kind = SheetValueKind::BOOLEAN;
	REQUIRE(SheetRawValueJson(v) == "false");
	v.kind = SheetValueKind::DATE;
	v.days = 0;
	REQUIRE(SheetRawValueJson(v) == "25569");
}

TEST_CASE("number formats only for the date kinds", "[sheets]") {
	REQUIRE(SheetNumberFormatJson(SheetValueKind::DATE) == R"({"type":"DATE","pattern":"yyyy-mm-dd"})");
	REQUIRE(SheetNumberFormatJson(SheetValueKind::DATE_TIME).find("DATE_TIME") != std::string::npos);
	REQUIRE(SheetNumberFormatJson(SheetValueKind::TIME).find("hh:mm:ss") != std::string::npos);
	REQUIRE(SheetNumberFormatJson(SheetValueKind::NUMBER).empty());
}

TEST_CASE("batchUpdate request shapes", "[sheets]") {
	std::vector<std::string> rows = {R"({"values":[{}]})", R"({"values":[{},{}]})", R"({"values":[]})"};
	REQUIRE(SheetUpdateCellsRequest(7, 1, 2, rows, 1, 3) ==
	        R"({"updateCells":{"start":{"sheetId":7,"rowIndex":1,"columnIndex":2},"rows":[{"values":[{},{}]},{"values":[]}],"fields":"userEnteredValue"}})");

	SheetGridRange open;
	open.start_row = 2;
	open.start_col = 1;
	REQUIRE(SheetClearValuesRequest(0, open) ==
	        R"({"updateCells":{"range":{"sheetId":0,"startRowIndex":2,"startColumnIndex":1},"fields":"userEnteredValue"}})");
	SheetGridRange bounded = open;
	bounded.end_row = 10;
	bounded.end_col = 4;
	REQUIRE(SheetClearValuesRequest(3, bounded).find(R"("endRowIndex":10,"endColumnIndex":4)") != std::string::npos);

	REQUIRE(SheetNumberFormatRequest(3, 1, 5, 2, SheetValueKind::DATE) ==
	        R"({"repeatCell":{"range":{"sheetId":3,"startRowIndex":1,"startColumnIndex":2,"endRowIndex":5,"endColumnIndex":3},"cell":{"userEnteredFormat":{"numberFormat":{"type":"DATE","pattern":"yyyy-mm-dd"}}},"fields":"userEnteredFormat.numberFormat"}})");

	REQUIRE(SheetAddSheetRequest(99, "It's \"new\"", 1000, 26) ==
	        R"({"addSheet":{"properties":{"sheetId":99,"title":"It's \"new\"","gridProperties":{"rowCount":1000,"columnCount":26}}}})");
	REQUIRE(SheetResizeGridRequest(4, 5000, 30) ==
	        R"({"updateSheetProperties":{"properties":{"sheetId":4,"gridProperties":{"rowCount":5000,"columnCount":30}},"fields":"gridProperties.rowCount,gridProperties.columnCount"}})");
	REQUIRE(SheetBatchUpdateBody({"{\"a\":1}", "{\"b\":2}"}) == R"({"requests":[{"a":1},{"b":2}]})");
	REQUIRE(SheetBatchUpdateBody({}) == R"({"requests":[]})");
}

TEST_CASE("a created Sheet's first tab: title and grid in one idempotent request", "[sheets]") {
	REQUIRE(SheetSetTabRequest(0, "It's \"Data\"", 1000, 26) ==
	        R"({"updateSheetProperties":{"properties":{"sheetId":0,"title":"It's \"Data\"","gridProperties":{"rowCount":1000,"columnCount":26}},"fields":"title,gridProperties.rowCount,gridProperties.columnCount"}})");
}

TEST_CASE("what a sheet_range COPY target is: open, create, or refuse", "[sheets]") {
	const std::string sheet = "application/vnd.google-apps.spreadsheet";
	const std::string folder = "application/vnd.google-apps.folder";
	using F = SheetTargetForm;
	using A = SheetTargetAction;
	// Existing objects, however addressed.
	for (auto form : {F::PATH, F::ID, F::ADDRESS}) {
		REQUIRE(DecideSheetTarget(true, sheet, form, true, "sales") == A::OPEN);
		REQUIRE(DecideSheetTarget(true, sheet, form, false, "sales") == A::OPEN);
		REQUIRE(DecideSheetTarget(true, folder, form, true, "sales") == A::REFUSE_NOT_SHEET);
		REQUIRE(DecideSheetTarget(true, "text/csv", form, true, "plain.csv") == A::REFUSE_NOT_SHEET);
	}
	// An existing Sheet named like a data file is still written into.
	REQUIRE(DecideSheetTarget(true, sheet, F::PATH, true, "export.csv") == A::OPEN);
	// Nothing there: only a path creates, and only when allowed.
	REQUIRE(DecideSheetTarget(false, "", F::PATH, true, "sales") == A::CREATE);
	REQUIRE(DecideSheetTarget(false, "", F::PATH, false, "sales") == A::NO_SUCH);
	REQUIRE(DecideSheetTarget(false, "", F::ID, true, "") == A::NO_SUCH);
	REQUIRE(DecideSheetTarget(false, "", F::ADDRESS, true, "") == A::NO_SUCH);
	REQUIRE(DecideSheetTarget(false, "", F::PATH, true, "") == A::NO_SUCH);
	// A data-file name is refused, any case -- unless CREATE false asked for
	// the plain "must exist" error.
	for (auto name : {"out.csv", "out.TSV", "x.parquet", "x.json", "x.jsonl", "x.ndjson", "x.xlsx", "x.XLS",
	                  "notes.txt"}) {
		INFO(name);
		REQUIRE(DecideSheetTarget(false, "", F::PATH, true, name) == A::REFUSE_DATA_EXTENSION);
		REQUIRE(DecideSheetTarget(false, "", F::PATH, false, name) == A::NO_SUCH);
	}
	// Not data extensions: a bare name, a dot elsewhere, the extension alone.
	for (auto name : {"report", "q3.plan", "csv", ".csv", "data.csv.bak", "v1.2"}) {
		INFO(name);
		REQUIRE(DecideSheetTarget(false, "", F::PATH, true, name) == A::CREATE);
	}
}

TEST_CASE("spreadsheet properties, and an unused sheetId", "[sheets]") {
	// The shape spreadsheets.get answers with fields=sheets.properties(...):
	// the first tab's sheetId 0 is omitted, as proto3 omits a default.
	const char *json = R"({"sheets":[
	  {"properties":{"title":"Sheet1","gridProperties":{"rowCount":1000,"columnCount":26}}},
	  {"properties":{"sheetId":1234,"title":"Forecast","gridProperties":{"rowCount":50,"columnCount":5}}}]})";
	std::vector<SheetProperties> props;
	std::string error;
	REQUIRE(ParseSheetProperties(json, props, error));
	REQUIRE(props.size() == 2);
	REQUIRE(props[0].sheet_id == 0);
	REQUIRE(props[0].rows == 1000);
	REQUIRE(props[1].sheet_id == 1234);
	REQUIRE(props[1].title == "Forecast");
	REQUIRE(props[1].cols == 5);

	REQUIRE(PickSheetId(props, 1234) == 1235);
	REQUIRE(PickSheetId(props, 0) == 1);
	REQUIRE(PickSheetId(props, 77) == 77);

	REQUIRE_FALSE(ParseSheetProperties("{}", props, error));
	REQUIRE_FALSE(ParseSheetProperties("<html>", props, error));
}

TEST_CASE("values.append's updated range", "[sheets]") {
	SheetRange r;
	std::string error;
	REQUIRE(ParseAppendedRange(
	    R"({"spreadsheetId":"x","tableRange":"Forecast!A1:C10","updates":{"updatedRange":"'My Tab'!A11:C13","updatedRows":3}})",
	    r, error));
	REQUIRE(r.sheet == "My Tab");
	REQUIRE(r.first_row == 11);
	REQUIRE(r.last_row == 13);
	REQUIRE(r.last_col == 3);
	REQUIRE_FALSE(ParseAppendedRange(R"({"updates":{}})", r, error));
}

TEST_CASE("grid data becomes typed cells; trailing empty rows drop", "[sheets]") {
	const char *json = R"({"sheets":[{"data":[{"rowData":[
	  {"values":[{"effectiveValue":{"stringValue":"day"}},{"effectiveValue":{"stringValue":"qty"}},{"effectiveValue":{"stringValue":"ok"}}]},
	  {"values":[{"effectiveValue":{"numberValue":46023},"effectiveFormat":{"numberFormat":{"type":"DATE","pattern":"yyyy-mm-dd"}}},
	             {"effectiveValue":{"numberValue":3}},{"effectiveValue":{"boolValue":true}}]},
	  {"values":[{"effectiveValue":{"numberValue":46024},"effectiveFormat":{"numberFormat":{"type":"DATE"}}},
	             {"effectiveValue":{"errorValue":{"type":"DIVIDE_BY_ZERO","message":"x"}}}]},
	  {"values":[{},{}]},
	  {}
	]}]}]})";
	std::vector<std::vector<SheetCell>> rows;
	std::string error;
	REQUIRE(ParseSheetGrid(json, 0, rows, error));
	REQUIRE(rows.size() == 3);
	REQUIRE(rows[0].size() == 3);
	REQUIRE(rows[2][1].kind == SheetCellKind::ERROR_CELL);
	REQUIRE(rows[2][1].text == "#DIV/0!");
	REQUIRE(rows[2][2].kind == SheetCellKind::EMPTY);
	REQUIRE(rows[1][0].format == SheetFormatKind::DATE);

	auto names = SheetHeaderNames(rows[0], 3);
	REQUIRE(names == std::vector<std::string> {"day", "qty", "ok"});

	std::vector<const SheetCell *> day = {&rows[1][0], &rows[2][0]};
	REQUIRE(InferSheetColumnType(day) == SheetColumnType::DATE);
	std::vector<const SheetCell *> qty = {&rows[1][1], &rows[2][1]};
	REQUIRE(InferSheetColumnType(qty) == SheetColumnType::BIGINT); // the error does not vote
	std::vector<const SheetCell *> ok = {&rows[1][2], &rows[2][2]};
	REQUIRE(InferSheetColumnType(ok) == SheetColumnType::BOOLEAN);

	// An explicit width pads and cuts.
	REQUIRE(ParseSheetGrid(json, 5, rows, error));
	REQUIRE(rows[0].size() == 5);
	REQUIRE(ParseSheetGrid(R"({"sheets":[{"data":[{}]}]})", 2, rows, error));
	REQUIRE(rows.empty());
	REQUIRE_FALSE(ParseSheetGrid(R"({"sheets":[]})", 0, rows, error));
}

TEST_CASE("column type inference follows read_excel_range's rules", "[sheets]") {
	auto num = [](double v, SheetFormatKind f = SheetFormatKind::OTHER) {
		SheetCell c;
		c.kind = SheetCellKind::NUMBER;
		c.number = v;
		c.format = f;
		return c;
	};
	SheetCell text;
	text.kind = SheetCellKind::STRING;
	text.text = "x";
	SheetCell yes;
	yes.kind = SheetCellKind::BOOLEAN;
	yes.boolean = true;
	auto a = num(1), b = num(2.5), d = num(46023, SheetFormatKind::DATE), dt = num(46023.5, SheetFormatKind::DATE_TIME),
	     t = num(0.5, SheetFormatKind::TIME);
	REQUIRE(InferSheetColumnType({&a}) == SheetColumnType::BIGINT);
	REQUIRE(InferSheetColumnType({&a, &b}) == SheetColumnType::DOUBLE);
	REQUIRE(InferSheetColumnType({&d, &dt}) == SheetColumnType::TIMESTAMP);
	REQUIRE(InferSheetColumnType({&t}) == SheetColumnType::TIME);
	REQUIRE(InferSheetColumnType({&d, &a}) == SheetColumnType::DOUBLE);
	REQUIRE(InferSheetColumnType({&a, &text}) == SheetColumnType::VARCHAR);
	REQUIRE(InferSheetColumnType({&a, &yes}) == SheetColumnType::VARCHAR);
	REQUIRE(InferSheetColumnType({}) == SheetColumnType::VARCHAR);
}

TEST_CASE("cell text for VARCHAR columns", "[sheets]") {
	SheetCell c;
	c.kind = SheetCellKind::NUMBER;
	c.number = 46023.5;
	c.format = SheetFormatKind::DATE_TIME;
	REQUIRE(SheetCellText(c) == "2026-01-01 12:00:00");
	c.format = SheetFormatKind::DATE;
	REQUIRE(SheetCellText(c) == "2026-01-01");
	c.format = SheetFormatKind::TIME;
	c.number = 0.25;
	REQUIRE(SheetCellText(c) == "06:00:00");
	c.format = SheetFormatKind::OTHER;
	c.number = 3;
	REQUIRE(SheetCellText(c) == "3");
	c.number = 0.1;
	REQUIRE(SheetCellText(c) == "0.1");
	c.kind = SheetCellKind::BOOLEAN;
	REQUIRE(SheetCellText(c) == "FALSE");
}

TEST_CASE("header names: trimmed, empty -> columnN, duplicates numbered", "[sheets]") {
	std::vector<SheetCell> header(4);
	header[0].kind = SheetCellKind::STRING;
	header[0].text = " Region ";
	header[2].kind = SheetCellKind::STRING;
	header[2].text = "region";
	auto names = SheetHeaderNames(header, 5);
	REQUIRE(names == std::vector<std::string> {"Region", "column2", "region_2", "column4", "column5"});
}
