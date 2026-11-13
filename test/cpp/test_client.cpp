// S-2.2 / S-2.5 / part of S-4.1 -- pure-logic coverage for gdrive_client.cpp.
//
// CLAUDE.md, decision D-1 / the two-layer test policy: GDriveClientImpl
// itself is DuckDB-coupled, real-network I/O and is exercised only by LIVE
// tests against real Drive (none of which can run yet -- no CI Shared Drive).
// This file tests only the genuinely pure pieces factored out inside
// gdrive_client.cpp: URL encoding, field-mask construction, Range header
// formatting, and ParseFileMeta/ParseFileList over literal JSON.
//
// CMakeLists.txt's PURE_SOURCES list is a fixed, explicit list of files (not
// a real glob) that we are not allowed to edit to add a new pure translation
// unit. Rather than duplicating gdrive_client.cpp's pure helpers into a
// second file, this test includes gdrive_client.cpp directly as a single
// translation unit -- gdrive_client.cpp has zero #include of duckdb.hpp (see
// its own file header comment), so this compiles and links here with no
// DuckDB linkage, using only httplib (header-only, include path already set
// up for this target), picojson and OpenSSL (both already linked for
// gdrive_unit_tests).
#include <catch2/catch_test_macros.hpp>

// See gdrive_client.cpp's file header comment: duckdb's vendored httplib.hpp
// needs duckdb linked in (duckdb::InternalException, duckdb_re2::Regex),
// which this pure-logic Catch2 binary does not do. This macro compiles out
// the networking section (GDriveClientImpl and everything that includes
// httplib.hpp), leaving only the pure pieces this file tests.
#define GDRIVE_CLIENT_PURE_ONLY
#include "../../src/gdrive_client.cpp"

using duckdb::gdrive::DriveCallStats;
using duckdb::gdrive::DriveFileMeta;
using duckdb::gdrive::ParseFileList;
using duckdb::gdrive::ParseFileMeta;

TEST_CASE("UrlEncode leaves unreserved characters alone", "[client]") {
	using duckdb::gdrive::internal::UrlEncode;
	REQUIRE(UrlEncode("abcXYZ019-_.~") == "abcXYZ019-_.~");
}

TEST_CASE("UrlEncode percent-encodes everything else", "[client]") {
	using duckdb::gdrive::internal::UrlEncode;
	REQUIRE(UrlEncode("a b") == "a%20b");
	REQUIRE(UrlEncode("'Finance 2026' in parents") ==
	        "%27Finance%202026%27%20in%20parents");
	REQUIRE(UrlEncode("a/b") == "a%2Fb");
	REQUIRE(UrlEncode("") == "");
}

TEST_CASE("FileFieldsMask is exactly the eight fields REQ-NF-01 allows", "[client]") {
	using duckdb::gdrive::internal::FileFieldsMask;
	// `trashed` since 2026-11: an id: path must not read a file in the trash.
	// contentRestrictions(readOnly) since 2026-11-13: a locked file must not
	// be replaced by COPY's tmp-file-then-move (RefuseReplacing).
	REQUIRE(FileFieldsMask() ==
	        "id,name,mimeType,size,modifiedTime,headRevisionId,trashed,contentRestrictions(readOnly)");
}

TEST_CASE("ListFieldsMask nests the same fields under files() with nextPageToken", "[client]") {
	using duckdb::gdrive::internal::ListFieldsMask;
	REQUIRE(ListFieldsMask() ==
	        "nextPageToken,files(id,name,mimeType,size,modifiedTime,headRevisionId,trashed,"
	        "contentRestrictions(readOnly))");
}

TEST_CASE("BuildRangeHeader is inclusive of both endpoints", "[client]") {
	using duckdb::gdrive::internal::BuildRangeHeader;
	REQUIRE(BuildRangeHeader(0, 0) == "bytes=0-0");
	REQUIRE(BuildRangeHeader(0, 99) == "bytes=0-99");
	REQUIRE(BuildRangeHeader(100, 199) == "bytes=100-199");
}

TEST_CASE("BuildRangeHeader with end < 0 means to EOF -- no end value at all", "[client]") {
	using duckdb::gdrive::internal::BuildRangeHeader;
	REQUIRE(BuildRangeHeader(500, -1) == "bytes=500-");
	REQUIRE(BuildRangeHeader(0, -1) == "bytes=0-");
}

TEST_CASE("AllDrivesParams sets both flags every time -- requirement 1", "[client]") {
	using duckdb::gdrive::internal::AllDrivesParams;
	REQUIRE(AllDrivesParams() == "supportsAllDrives=true&includeItemsFromAllDrives=true");
}

// ---------------------------------------------------------------------------
// ParseFileMeta
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileMeta parses a normal files.get response", "[client]") {
	const std::string json = R"({
	  "id": "1AbCdEf",
	  "name": "actuals.parquet",
	  "mimeType": "application/octet-stream",
	  "size": "123456",
	  "modifiedTime": "2026-07-01T12:00:00.000Z",
	  "headRevisionId": "0Br"
	})";
	DriveFileMeta meta;
	REQUIRE(ParseFileMeta(json, meta));
	REQUIRE(meta.id == "1AbCdEf");
	REQUIRE(meta.name == "actuals.parquet");
	REQUIRE(meta.mime_type == "application/octet-stream");
	REQUIRE(meta.size == 123456);
	REQUIRE(meta.modified_time == "2026-07-01T12:00:00.000Z");
	REQUIRE(meta.head_revision_id == "0Br");
	REQUIRE_FALSE(meta.IsFolder());
	REQUIRE_FALSE(meta.IsNativeGoogleFormat());
}

TEST_CASE("ParseFileMeta: size is a JSON string, not a number, per Drive's wire format", "[client]") {
	// A native Google format (a Sheet) reports no size at all.
	const std::string json = R"({
	  "id": "1Sheet",
	  "name": "Budget",
	  "mimeType": "application/vnd.google-apps.spreadsheet",
	  "modifiedTime": "2026-07-01T12:00:00.000Z",
	  "headRevisionId": "0Sh"
	})";
	DriveFileMeta meta;
	REQUIRE(ParseFileMeta(json, meta));
	REQUIRE(meta.size == -1);
	REQUIRE_FALSE(meta.IsFolder());
	REQUIRE(meta.IsNativeGoogleFormat());
}

TEST_CASE("ParseFileMeta recognises a folder but does not call it a native format", "[client]") {
	const std::string json = R"({
	  "id": "1Folder",
	  "name": "2026",
	  "mimeType": "application/vnd.google-apps.folder",
	  "modifiedTime": "2026-07-01T12:00:00.000Z",
	  "headRevisionId": "0Fo"
	})";
	DriveFileMeta meta;
	REQUIRE(ParseFileMeta(json, meta));
	REQUIRE(meta.IsFolder());
	REQUIRE_FALSE(meta.IsNativeGoogleFormat());
}

TEST_CASE("ParseFileMeta fails softly on garbage input", "[client]") {
	DriveFileMeta meta;
	REQUIRE_FALSE(ParseFileMeta("", meta));
	REQUIRE_FALSE(ParseFileMeta("not json at all", meta));
	REQUIRE_FALSE(ParseFileMeta("[1,2,3]", meta));
	REQUIRE_FALSE(ParseFileMeta("{}", meta)); // no `id`
}

// ---------------------------------------------------------------------------
// ParseFileList
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileList parses a normal files.list page with a next page token", "[client]") {
	const std::string json = R"({
	  "nextPageToken": "abc123",
	  "files": [
	    {"id": "1", "name": "a.csv", "mimeType": "text/csv", "size": "10",
	     "modifiedTime": "2026-01-01T00:00:00.000Z", "headRevisionId": "r1"},
	    {"id": "2", "name": "b.csv", "mimeType": "text/csv", "size": "20",
	     "modifiedTime": "2026-01-02T00:00:00.000Z", "headRevisionId": "r2"}
	  ]
	})";
	std::vector<DriveFileMeta> files;
	std::string next_token;
	REQUIRE(ParseFileList(json, files, next_token));
	REQUIRE(files.size() == 2);
	REQUIRE(files[0].id == "1");
	REQUIRE(files[1].id == "2");
	REQUIRE(next_token == "abc123");
}

TEST_CASE("ParseFileList on the last page has no nextPageToken", "[client]") {
	const std::string json = R"({"files": [{"id": "1", "name": "a.csv", "mimeType": "text/csv"}]})";
	std::vector<DriveFileMeta> files;
	std::string next_token;
	REQUIRE(ParseFileList(json, files, next_token));
	REQUIRE(files.size() == 1);
	REQUIRE(next_token.empty());
}

TEST_CASE("ParseFileList treats a missing `files` key as a well-formed empty listing", "[client]") {
	const std::string json = R"({"kind": "drive#fileList"})";
	std::vector<DriveFileMeta> files;
	std::string next_token;
	REQUIRE(ParseFileList(json, files, next_token));
	REQUIRE(files.empty());
}

TEST_CASE("ParseFileList fails softly on garbage input", "[client]") {
	std::vector<DriveFileMeta> files;
	std::string next_token;
	REQUIRE_FALSE(ParseFileList("", files, next_token));
	REQUIRE_FALSE(ParseFileList("not json", files, next_token));
	REQUIRE_FALSE(ParseFileList(R"({"files": "not an array"})", files, next_token));
}

// ---------------------------------------------------------------------------
// DriveCallStats::Total()
// ---------------------------------------------------------------------------

TEST_CASE("DriveCallStats::Total sums the per-kind call counters only", "[client]") {
	DriveCallStats stats;
	stats.files_get = 1;
	stats.files_list = 2;
	stats.files_media = 3;
	stats.files_export = 4;
	stats.files_create = 5;
	stats.files_update = 6;
	stats.files_delete = 7;
	stats.generate_ids = 8;
	// Neither of these should count toward Total(): cache hits/misses are not
	// Drive API calls, and a retry is already reflected once in whichever kind
	// counter it belongs to.
	stats.cache_hits = 100;
	stats.cache_misses = 200;
	stats.retries = 300;
	REQUIRE(stats.Total() == 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8);
}

// ---------------------------------------------------------------------------
// files.generateIds -- the reservation that makes a CREATE retryable.
//
// Verified against the live API 2026-07-27: creating with a reserved id
// succeeds once and returns 409 "A file already exists with the provided ID"
// on a repeat, with exactly ONE file present. That 409 is what lets an
// ambiguous transport failure be retried without risking the duplicate that
// R-4 turns into a permanently unaddressable path. 2026-11-13: the same holds
// for folders, so a write into a new subtree reserves the whole chain (and
// the file) in one generateIds?count=N.
// ---------------------------------------------------------------------------
TEST_CASE("ParseGeneratedFileIds takes exactly the ids asked for", "[client]") {
	using duckdb::gdrive::ParseGeneratedFileIds;
	std::vector<std::string> ids;
	// A real generateIds?count=3 reply, captured 2026-11-13.
	const char *reply = R"({
 "kind": "drive#generatedIds",
 "space": "drive",
 "ids": ["1WiIcNJfCo48kJM_zmqsVom6rtjoIK8hQ", "1NVUSS75Mh4OvAVXgbNNEMwKvhTsepTrj", "1Np3XIfyZ-OVpH_F4CyUGDLdr8uZavEhG"]
})";
	REQUIRE(ParseGeneratedFileIds(reply, 3, ids));
	REQUIRE(ids.size() == 3);
	REQUIRE(ids[0] == "1WiIcNJfCo48kJM_zmqsVom6rtjoIK8hQ");
	REQUIRE(ids[2] == "1Np3XIfyZ-OVpH_F4CyUGDLdr8uZavEhG");
	// Fewer (or more) than asked would leave a create in the chain without an
	// id. Refused, not truncated.
	REQUIRE_FALSE(ParseGeneratedFileIds(reply, 4, ids));
	REQUIRE(ids.empty());
	REQUIRE_FALSE(ParseGeneratedFileIds(reply, 2, ids));
}

TEST_CASE("ParseGeneratedFileIds rejects responses that carry no usable id", "[client]") {
	using duckdb::gdrive::ParseGeneratedFileIds;
	std::vector<std::string> ids;
	// An empty id would be sent as "no reservation" -- silently losing the
	// idempotency this exists for.
	REQUIRE_FALSE(ParseGeneratedFileIds(R"({"ids":[]})", 1, ids));
	REQUIRE_FALSE(ParseGeneratedFileIds(R"({"ids":[""]})", 1, ids));
	REQUIRE_FALSE(ParseGeneratedFileIds(R"({"ids":["a",""]})", 2, ids));
	REQUIRE_FALSE(ParseGeneratedFileIds(R"({})", 1, ids));
	REQUIRE_FALSE(ParseGeneratedFileIds("not json at all", 1, ids));
	// An error body must never look like a reservation.
	REQUIRE_FALSE(ParseGeneratedFileIds(R"({"error":{"code":403}})", 1, ids));
}

TEST_CASE("ParseNameAndParents reads a moved file's state back", "[client]") {
	std::string name;
	std::vector<std::string> parents;
	REQUIRE(duckdb::gdrive::ParseNameAndParents(
	    R"({"id": "1vV0", "name": "y.csv", "parents": ["113J5RrPURM_GPZgYvZt2Mk0-uFUg0rkl"]})", name, parents));
	REQUIRE(name == "y.csv");
	REQUIRE(parents == std::vector<std::string> {"113J5RrPURM_GPZgYvZt2Mk0-uFUg0rkl"});
	REQUIRE(duckdb::gdrive::ParseNameAndParents(R"({"id": "1", "name": "n"})", name, parents));
	REQUIRE(parents.empty());
	REQUIRE_FALSE(duckdb::gdrive::ParseNameAndParents(R"({"error":{"code":404}})", name, parents));
}
