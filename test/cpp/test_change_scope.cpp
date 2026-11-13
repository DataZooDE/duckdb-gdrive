// Whether a changed file is under the folder gdrive_changes watches, and at
// what path. Pure logic, no DuckDB linkage: the folder lookups are a callback,
// so the tree below is plain data.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "gdrive_change_scope.hpp"
#include "gdrive_rows.hpp"

using duckdb::gdrive::ChangeScope;
using duckdb::gdrive::ChangeScopeNode;

namespace {

//! root
//!  +- F (the watched folder)
//!  |   +- a
//!  |   |   +- b
//!  |   +- c
//!  +- elsewhere
std::map<std::string, ChangeScopeNode> Tree() {
	return {{"root", {"My Drive", {}}},  {"F", {"Finance", {"root"}}}, {"a", {"2026", {"F"}}},
	        {"b", {"q4", {"a"}}},         {"c", {"archive", {"F"}}},  {"elsewhere", {"Other", {"root"}}}};
}

ChangeScope ScopeOver(const std::map<std::string, ChangeScopeNode> &tree, const std::string &folder) {
	return ChangeScope(folder, [tree](const std::string &id, ChangeScopeNode &node) {
		auto it = tree.find(id);
		if (it == tree.end()) {
			return false;
		}
		node = it->second;
		return true;
	});
}

} // namespace

TEST_CASE("ChangeScope: files under the folder get their path below it", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel;
	REQUIRE(scope.Locate("top.csv", {"F"}, rel));
	CHECK(rel == "top.csv");
	REQUIRE(scope.Locate("deep.csv", {"b"}, rel));
	CHECK(rel == "2026/q4/deep.csv");
	REQUIRE(scope.Locate("old.csv", {"c"}, rel));
	CHECK(rel == "archive/old.csv");
}

TEST_CASE("ChangeScope: outside the folder, or above it, is out of scope", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel = "unchanged";
	CHECK_FALSE(scope.Locate("x.csv", {"elsewhere"}, rel));
	CHECK_FALSE(scope.Locate("x.csv", {"root"}, rel));
	CHECK_FALSE(scope.Locate("orphan.csv", {}, rel));
	// A parent this identity cannot see: out, not an error.
	CHECK_FALSE(scope.Locate("x.csv", {"unshared"}, rel));
	CHECK(rel == "unchanged");
}

TEST_CASE("ChangeScope: each folder is looked up once", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel;
	REQUIRE(scope.Locate("1.csv", {"b"}, rel));
	const auto first = scope.Lookups();
	CHECK(first == 2); // b, then a; F is known
	for (int i = 0; i < 50; i++) {
		REQUIRE(scope.Locate("n.csv", {"b"}, rel));
		REQUIRE(scope.Locate("n.csv", {"a"}, rel));
	}
	CHECK(scope.Lookups() == first);
	CHECK_FALSE(scope.Locate("x.csv", {"elsewhere"}, rel));
	CHECK(scope.Lookups() == first + 2); // elsewhere, root
	CHECK_FALSE(scope.Locate("y.csv", {"elsewhere"}, rel));
	CHECK(scope.Lookups() == first + 2);
}

TEST_CASE("ChangeScope: any parent that leads to the folder will do", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel;
	REQUIRE(scope.Locate("shared.csv", {"elsewhere", "c"}, rel));
	CHECK(rel == "archive/shared.csv");
}

TEST_CASE("ChangeScope: a parent cycle ends instead of recursing", "[changes][scope]") {
	std::map<std::string, ChangeScopeNode> tree {{"x", {"x", {"y"}}}, {"y", {"y", {"x"}}}};
	auto scope = ScopeOver(tree, "F");
	std::string rel;
	CHECK_FALSE(scope.Locate("f.csv", {"x"}, rel));
	CHECK(scope.Lookups() == 2);
}

TEST_CASE("ChangeScope: a failing lookup propagates", "[changes][scope]") {
	ChangeScope scope("F", [](const std::string &, ChangeScopeNode &) -> bool { throw std::runtime_error("403"); });
	std::string rel;
	CHECK_THROWS_AS(scope.Locate("f.csv", {"a"}, rel), std::runtime_error);
}

TEST_CASE("ChangeScope: a '/' in a name makes the path unrepresentable", "[changes][scope]") {
	auto tree = Tree();
	tree["s"] = ChangeScopeNode {"a/b", {"F"}};
	auto scope = ScopeOver(tree, "F");
	std::string rel;
	bool representable = false;
	REQUIRE(scope.Locate("x.csv", {"a"}, rel, &representable));
	CHECK(representable);
	REQUIRE(scope.Locate("x.csv", {"s"}, rel, &representable));
	CHECK_FALSE(representable); // under a folder named "a/b"
	REQUIRE(scope.Locate("q/4.csv", {"F"}, rel, &representable));
	CHECK_FALSE(representable); // its own name
}

// ---------------------------------------------------------------------------
// Confirm before delete: DecideChange over the whole table, and the captured
// bodies that drive it.
// ---------------------------------------------------------------------------

using duckdb::gdrive::ChangeEntryKind;
using duckdb::gdrive::ChangeLookup;
using duckdb::gdrive::ChangeRow;
using duckdb::gdrive::ChangeVerdict;
using duckdb::gdrive::ClassifyChangeEntry;
using duckdb::gdrive::ConfirmReply;
using duckdb::gdrive::DecideChange;
using duckdb::gdrive::FileRow;
using duckdb::gdrive::InterpretConfirmReply;
using duckdb::gdrive::ParseChanges;

TEST_CASE("ClassifyChangeEntry: removed first, then the file record, then its parents", "[changes][confirm]") {
	// removed wins over anything the entry might also carry
	CHECK(ClassifyChangeEntry(true, true, true, false) == ChangeEntryKind::REMOVED);
	CHECK(ClassifyChangeEntry(true, false, false, false) == ChangeEntryKind::REMOVED);
	CHECK(ClassifyChangeEntry(false, false, false, false) == ChangeEntryKind::NO_FILE);
	CHECK(ClassifyChangeEntry(false, true, false, false) == ChangeEntryKind::NO_PARENTS);
	// trashed WITHOUT parents cannot be placed either: it is confirmed too
	CHECK(ClassifyChangeEntry(false, true, false, true) == ChangeEntryKind::NO_PARENTS);
	CHECK(ClassifyChangeEntry(false, true, true, true) == ChangeEntryKind::TRASHED);
	CHECK(ClassifyChangeEntry(false, true, true, false) == ChangeEntryKind::PLACEABLE);
}

TEST_CASE("DecideChange: entries with parents keep today's placement, at no extra cost", "[changes][confirm]") {
	const ChangeLookup gone;
	for (bool outside : {false, true}) {
		CHECK(DecideChange(ChangeEntryKind::TRASHED, nullptr, outside) == ChangeVerdict::KEEP_AS_IS);
		CHECK(DecideChange(ChangeEntryKind::PLACEABLE, nullptr, outside) == ChangeVerdict::KEEP_AS_IS);
		// ...and a lookup, were one made, would not change that
		CHECK(DecideChange(ChangeEntryKind::PLACEABLE, &gone, outside) == ChangeVerdict::KEEP_AS_IS);
	}
}

TEST_CASE("DecideChange: the whole table for entries that cannot be placed", "[changes][confirm]") {
	ChangeLookup gone, alive_in, alive_out, trashed_in, trashed_out;
	alive_in.found = alive_out.found = trashed_in.found = trashed_out.found = true;
	alive_in.located = trashed_in.located = true;
	trashed_in.trashed = trashed_out.trashed = true;
	using K = ChangeEntryKind;
	using V = ChangeVerdict;

	struct Row {
		K kind;
		const ChangeLookup *lookup;
		bool outside;
		V want;
	};
	const Row table[] = {
	    // No lookup yet: removed / file-less entries always ask for one; an
	    // entry without parents only with outside -- without it, it emits
	    // nothing and costs nothing (typically shared-with-me items).
	    {K::REMOVED, nullptr, false, V::LOOK_UP},
	    {K::REMOVED, nullptr, true, V::LOOK_UP},
	    {K::NO_FILE, nullptr, false, V::LOOK_UP},
	    {K::NO_FILE, nullptr, true, V::LOOK_UP},
	    {K::NO_PARENTS, nullptr, false, V::NOTHING},
	    {K::NO_PARENTS, nullptr, true, V::LOOK_UP},
	    // Removed / no file record: a delete unless Drive places it under the folder.
	    {K::REMOVED, &gone, false, V::DELETE},
	    {K::REMOVED, &gone, true, V::DELETE},
	    {K::REMOVED, &alive_in, false, V::UPSERT_FROM_LOOKUP},
	    {K::REMOVED, &alive_in, true, V::UPSERT_FROM_LOOKUP},
	    {K::REMOVED, &alive_out, false, V::DELETE}, // may have moved out: still dropped
	    {K::REMOVED, &alive_out, true, V::DELETE},
	    {K::REMOVED, &trashed_in, false, V::DELETE_AT_PATH},
	    {K::REMOVED, &trashed_in, true, V::DELETE_AT_PATH},
	    {K::REMOVED, &trashed_out, false, V::DELETE},
	    {K::REMOVED, &trashed_out, true, V::DELETE},
	    {K::NO_FILE, &gone, false, V::DELETE},
	    {K::NO_FILE, &gone, true, V::DELETE},
	    {K::NO_FILE, &alive_in, false, V::UPSERT_FROM_LOOKUP},
	    {K::NO_FILE, &alive_in, true, V::UPSERT_FROM_LOOKUP},
	    {K::NO_FILE, &alive_out, false, V::DELETE},
	    {K::NO_FILE, &alive_out, true, V::DELETE},
	    {K::NO_FILE, &trashed_in, false, V::DELETE_AT_PATH},
	    {K::NO_FILE, &trashed_in, true, V::DELETE_AT_PATH},
	    {K::NO_FILE, &trashed_out, false, V::DELETE},
	    {K::NO_FILE, &trashed_out, true, V::DELETE},
	    // No parents, with outside (the only mode that looks it up): placed
	    // nowhere stays a delete; placed under the folder is believed.
	    {K::NO_PARENTS, &gone, true, V::DELETE},
	    {K::NO_PARENTS, &alive_in, true, V::UPSERT_FROM_LOOKUP},
	    {K::NO_PARENTS, &alive_out, true, V::DELETE},
	    {K::NO_PARENTS, &trashed_in, true, V::DELETE_AT_PATH},
	    {K::NO_PARENTS, &trashed_out, true, V::DELETE},
	    // ...and without outside it is NOTHING whatever a lookup would say.
	    {K::NO_PARENTS, &gone, false, V::NOTHING},
	    {K::NO_PARENTS, &alive_in, false, V::NOTHING},
	    {K::NO_PARENTS, &alive_out, false, V::NOTHING},
	    {K::NO_PARENTS, &trashed_in, false, V::NOTHING},
	    {K::NO_PARENTS, &trashed_out, false, V::NOTHING},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		INFO("row " << i);
		CHECK(DecideChange(table[i].kind, table[i].lookup, table[i].outside) == table[i].want);
	}
}

namespace {

// CAPTURED 2026-11-13 -- from test/cpp/testdata/changes_feed.json
// ("removed_entry_page"): a file of a scratch folder, permanently deleted.
const char *kRemovedEntryPage = R"({
  "changes": [
    {
      "changeType": "file",
      "time": "2026-11-13T15:39:51.314Z",
      "removed": true,
      "fileId": "1BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
    }
  ],
  "newStartPageToken": "100000"
})";

// CAPTURED 2026-11-13 -- verbatim from test/cpp/testdata/drive_errors.json
// ("not_found_deleted_get"): files.get on that same id.
const char *kDeletedGet = R"({
  "error": {
    "code": 404,
    "message": "File not found: 1BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB.",
    "errors": [
      {
        "message": "File not found: 1BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB.",
        "domain": "global",
        "reason": "notFound",
        "location": "fileId",
        "locationType": "parameter"
      }
    ]
  }
}
)";

// CAPTURED 2026-11-13 -- from test/cpp/testdata/changes_feed.json
// ("confirm_get_live" / "confirm_get_trashed"): files.get with FileRowFields().
const char *kLiveGet = R"({
  "id": "1CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
  "name": "live.csv",
  "mimeType": "text/csv",
  "starred": false,
  "trashed": false,
  "parents": [
    "1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
  ],
  "webViewLink": "https://drive.google.com/file/d/1CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC/view?usp=drivesdk",
  "createdTime": "2026-11-13T15:42:30.796Z",
  "modifiedTime": "2026-11-13T15:42:30.796Z",
  "owners": [{"displayName": "owner", "emailAddress": "owner@example.com"}],
  "lastModifyingUser": {"displayName": "owner", "emailAddress": "owner@example.com"},
  "shared": true,
  "copyRequiresWriterPermission": false,
  "md5Checksum": "dc3ba59be5d893be2c69ea202014fe78",
  "size": "4",
  "headRevisionId": "0B9fS7-FFsZTlMlE5WFFERFhYbHMvQWl1NFpXdmFzL01MODRrPQ"
})";

const char *kTrashedGet = R"({
  "id": "1DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
  "name": "binned.csv",
  "mimeType": "text/csv",
  "starred": false,
  "trashed": true,
  "parents": [
    "1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
  ],
  "webViewLink": "https://drive.google.com/file/d/1DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD/view?usp=drivesdk",
  "createdTime": "2026-11-13T15:42:32.815Z",
  "modifiedTime": "2026-11-13T15:42:32.815Z",
  "owners": [{"displayName": "owner", "emailAddress": "owner@example.com"}],
  "lastModifyingUser": {"displayName": "owner", "emailAddress": "owner@example.com"},
  "shared": true,
  "copyRequiresWriterPermission": false,
  "md5Checksum": "dc3ba59be5d893be2c69ea202014fe78",
  "size": "4",
  "headRevisionId": "0B9fS7-FFsZTlZUxCS1J1SjdKY052bWJtTjJ3dW0yR1FpS0w4PQ"
})";

//! The captured files' parent is the watched folder.
const char *kCapturedFolder = "1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";

//! The first entry of a page, classified, confirmed with a reply, decided --
//! the path gdrive_changes takes, minus the I/O.
ChangeVerdict Pipeline(const char *page, int status, const char *reply, bool outside) {
	std::vector<ChangeRow> rows;
	std::string next, start, error;
	REQUIRE(ParseChanges(page, rows, next, start, error));
	REQUIRE(rows.size() == 1);
	const auto &c = rows[0];
	const auto kind = ClassifyChangeEntry(c.removed, c.has_file, !c.file.parents.empty(), c.file.trashed);
	REQUIRE(DecideChange(kind, nullptr, outside) == ChangeVerdict::LOOK_UP);
	FileRow file;
	ChangeLookup lookup;
	const auto answer = InterpretConfirmReply(status, reply, file, error);
	REQUIRE(answer != ConfirmReply::FAILED);
	if (answer == ConfirmReply::FOUND) {
		ChangeScope scope(kCapturedFolder, [](const std::string &, ChangeScopeNode &) { return false; });
		std::string rel;
		lookup.found = true;
		lookup.trashed = file.trashed;
		lookup.located = scope.Locate(file.name, file.parents, rel);
	}
	return DecideChange(kind, &lookup, outside);
}

} // namespace

TEST_CASE("Confirm (captured): a permanently deleted file is removed:true, then 404 -- a delete",
          "[changes][confirm]") {
	std::vector<ChangeRow> rows;
	std::string next, start, error;
	REQUIRE(ParseChanges(kRemovedEntryPage, rows, next, start, error));
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].removed);
	CHECK_FALSE(rows[0].has_file); // Drive says only the id
	CHECK(start == "100000");

	FileRow file;
	CHECK(InterpretConfirmReply(404, kDeletedGet, file, error) == ConfirmReply::GONE);
	for (bool outside : {false, true}) {
		CHECK(Pipeline(kRemovedEntryPage, 404, kDeletedGet, outside) == ChangeVerdict::DELETE);
	}
}

TEST_CASE("Confirm (captured): a removed entry for a file Drive still shows under the folder is an upsert",
          "[changes][confirm]") {
	FileRow file;
	std::string error;
	REQUIRE(InterpretConfirmReply(200, kLiveGet, file, error) == ConfirmReply::FOUND);
	// The upsert row is built from files.get: everything such a row carries.
	CHECK(file.id == "1CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC");
	CHECK(file.name == "live.csv");
	CHECK(file.mime_type == "text/csv");
	CHECK(file.size == 4);
	CHECK(file.modified == "2026-11-13T15:42:30.796Z");
	CHECK(file.modified_by.email == "owner@example.com");
	REQUIRE(file.parents.size() == 1);
	CHECK(file.parents[0] == kCapturedFolder);
	CHECK_FALSE(file.trashed);
	for (bool outside : {false, true}) {
		CHECK(Pipeline(kRemovedEntryPage, 200, kLiveGet, outside) == ChangeVerdict::UPSERT_FROM_LOOKUP);
		CHECK(Pipeline(kRemovedEntryPage, 200, kTrashedGet, outside) == ChangeVerdict::DELETE_AT_PATH);
	}
}

TEST_CASE("InterpretConfirmReply: only a 404 means gone", "[changes][confirm]") {
	FileRow file;
	std::string error;
	CHECK(InterpretConfirmReply(404, kDeletedGet, file, error) == ConfirmReply::GONE);
	// A 200 that is not a file record fails; it is not "gone".
	CHECK(InterpretConfirmReply(200, "[1,2]", file, error) == ConfirmReply::FAILED);
	CHECK_FALSE(error.empty());
	// A transport failure, a 5xx or a 403 must never read as "gone": that
	// would turn a network blip into a delete of a live file.
	CHECK(InterpretConfirmReply(0, "", file, error) == ConfirmReply::FAILED);
	CHECK(InterpretConfirmReply(503, "<html>busy</html>", file, error) == ConfirmReply::FAILED);
	CHECK(InterpretConfirmReply(403, "", file, error) == ConfirmReply::FAILED);
}
