// S-2.1 -- gdrive:// URI parsing. Pure logic, no DuckDB linkage.
#include <catch2/catch_test_macros.hpp>

#include "gdrive_uri.hpp"

using duckdb::gdrive::GDriveUri;
using duckdb::gdrive::GDriveUriKind;
using duckdb::gdrive::GDriveUriParse;
using duckdb::gdrive::IsGDriveUri;
using duckdb::gdrive::ParseGDriveUri;

// ---------------------------------------------------------------------------
// IsGDriveUri -- cheap prefix test
// ---------------------------------------------------------------------------

TEST_CASE("IsGDriveUri accepts the scheme prefix", "[uri][is_gdrive]") {
	REQUIRE(IsGDriveUri("gdrive://Finance/2026/actuals.parquet"));
	REQUIRE(IsGDriveUri("gdrive://id:1a2b3c"));
	REQUIRE(IsGDriveUri("gdrive://"));
}

TEST_CASE("IsGDriveUri rejects other schemes and malformed prefixes", "[uri][is_gdrive]") {
	REQUIRE_FALSE(IsGDriveUri("s3://x"));
	REQUIRE_FALSE(IsGDriveUri("gdrive:/x"));
	REQUIRE_FALSE(IsGDriveUri("gdrive:x"));
	REQUIRE_FALSE(IsGDriveUri("foo"));
	REQUIRE_FALSE(IsGDriveUri("GDRIVE://x")); // case-sensitive
}

TEST_CASE("IsGDriveUri handles edge lengths without allocating or throwing", "[uri][is_gdrive]") {
	REQUIRE_FALSE(IsGDriveUri(""));
	REQUIRE_FALSE(IsGDriveUri("g"));
	REQUIRE_FALSE(IsGDriveUri("gdrive"));
	REQUIRE_FALSE(IsGDriveUri("gdrive:/"));
	REQUIRE_FALSE(IsGDriveUri("gdrive:/")); // shorter than full prefix by one char
}

// ---------------------------------------------------------------------------
// ParseGDriveUri -- happy paths
// ---------------------------------------------------------------------------

TEST_CASE("parses a multi-segment path", "[uri][parse][path]") {
	auto r = ParseGDriveUri("gdrive://Finance/2026/actuals.parquet");
	REQUIRE(r.ok);
	REQUIRE(r.error.empty());
	REQUIRE(r.uri.kind == GDriveUriKind::PATH);
	REQUIRE(r.uri.segments == std::vector<std::string>{"Finance", "2026", "actuals.parquet"});
}

TEST_CASE("parses the file id form", "[uri][parse][file_id]") {
	auto r = ParseGDriveUri("gdrive://id:1a2b3c");
	REQUIRE(r.ok);
	REQUIRE(r.uri.kind == GDriveUriKind::FILE_ID);
	REQUIRE(r.uri.file_id == "1a2b3c");
}

TEST_CASE("empty id: is an error", "[uri][parse][file_id]") {
	auto r = ParseGDriveUri("gdrive://id:");
	REQUIRE_FALSE(r.ok);
	REQUIRE_FALSE(r.error.empty());
}

TEST_CASE("bare scheme is the drive root: zero segments", "[uri][parse][root]") {
	auto r = ParseGDriveUri("gdrive://");
	REQUIRE(r.ok);
	REQUIRE(r.uri.kind == GDriveUriKind::PATH);
	REQUIRE(r.uri.segments.empty());
}

TEST_CASE("trailing slash is accepted and ignored", "[uri][parse][slashes]") {
	auto r = ParseGDriveUri("gdrive://Finance/2026/");
	REQUIRE(r.ok);
	REQUIRE(r.uri.segments == std::vector<std::string>{"Finance", "2026"});

	auto root = ParseGDriveUri("gdrive:///");
	REQUIRE(root.ok);
	REQUIRE(root.uri.segments.empty());
}

TEST_CASE("repeated slashes collapse", "[uri][parse][slashes]") {
	auto a = ParseGDriveUri("gdrive://a//b");
	auto b = ParseGDriveUri("gdrive://a/b");
	REQUIRE(a.ok);
	REQUIRE(b.ok);
	REQUIRE(a.uri.segments == b.uri.segments);
	REQUIRE(a.uri.segments == std::vector<std::string>{"a", "b"});
}

TEST_CASE("many repeated slashes still collapse", "[uri][parse][slashes]") {
	auto r = ParseGDriveUri("gdrive://a////b///c.csv");
	REQUIRE(r.ok);
	REQUIRE(r.uri.segments == std::vector<std::string>{"a", "b", "c.csv"});
}

// ---------------------------------------------------------------------------
// ParseGDriveUri -- rejected segments
// ---------------------------------------------------------------------------

TEST_CASE("dot-dot segment is rejected with an explanatory message", "[uri][parse][reject]") {
	auto r = ParseGDriveUri("gdrive://a/../b");
	REQUIRE_FALSE(r.ok);
	REQUIRE_FALSE(r.error.empty());
	// Message should explain why -- Drive has no parent-traversal concept.
	CAPTURE(r.error);
	REQUIRE((r.error.find("..") != std::string::npos || r.error.find("parent") != std::string::npos ||
	         r.error.find("traversal") != std::string::npos));
}

TEST_CASE("dot segment is rejected with an explanatory message", "[uri][parse][reject]") {
	auto r = ParseGDriveUri("gdrive://a/./b");
	REQUIRE_FALSE(r.ok);
	REQUIRE_FALSE(r.error.empty());
	CAPTURE(r.error);
}

TEST_CASE("leading dot-dot is rejected", "[uri][parse][reject]") {
	auto r = ParseGDriveUri("gdrive://../etc");
	REQUIRE_FALSE(r.ok);
}

TEST_CASE("trailing dot-dot is rejected", "[uri][parse][reject]") {
	auto r = ParseGDriveUri("gdrive://a/..");
	REQUIRE_FALSE(r.ok);
}

// ---------------------------------------------------------------------------
// ParseGDriveUri -- wrong scheme
// ---------------------------------------------------------------------------

TEST_CASE("non-gdrive schemes are rejected", "[uri][parse][scheme]") {
	REQUIRE_FALSE(ParseGDriveUri("s3://x").ok);
	REQUIRE_FALSE(ParseGDriveUri("gdrive:/x").ok);
	REQUIRE_FALSE(ParseGDriveUri("gdrive:x").ok);
	REQUIRE_FALSE(ParseGDriveUri("foo").ok);
	REQUIRE_FALSE(ParseGDriveUri("").ok);
}

// ---------------------------------------------------------------------------
// ParseGDriveUri -- names must survive verbatim (no percent-decoding)
// ---------------------------------------------------------------------------

TEST_CASE("names containing spaces, UTF-8, percent, plus, hash, question mark survive verbatim",
          "[uri][parse][verbatim]") {
	auto r = ParseGDriveUri("gdrive://My Folder/grüße.csv");
	REQUIRE(r.ok);
	REQUIRE(r.uri.segments == std::vector<std::string>{"My Folder", "grüße.csv"});

	auto pct = ParseGDriveUri("gdrive://100%25 done.csv");
	REQUIRE(pct.ok);
	REQUIRE(pct.uri.segments == std::vector<std::string>{"100%25 done.csv"});

	auto plus = ParseGDriveUri("gdrive://a+b.csv");
	REQUIRE(plus.ok);
	REQUIRE(plus.uri.segments == std::vector<std::string>{"a+b.csv"});

	auto hash = ParseGDriveUri("gdrive://report#1.csv");
	REQUIRE(hash.ok);
	REQUIRE(hash.uri.segments == std::vector<std::string>{"report#1.csv"});

	auto q = ParseGDriveUri("gdrive://what?.csv");
	REQUIRE(q.ok);
	REQUIRE(q.uri.segments == std::vector<std::string>{"what?.csv"});
}

// ---------------------------------------------------------------------------
// "id:" is only special as the *whole path* (a single first segment).
// A name that itself starts with "id:" but sits deeper in the path, or
// alongside other segments, is just an ordinary path segment.
// ---------------------------------------------------------------------------

TEST_CASE("id: prefix deeper in the path is an ordinary segment, not FILE_ID", "[uri][parse][id_edge]") {
	auto r = ParseGDriveUri("gdrive://folder/id:weird");
	REQUIRE(r.ok);
	REQUIRE(r.uri.kind == GDriveUriKind::PATH);
	REQUIRE(r.uri.segments == std::vector<std::string>{"folder", "id:weird"});
}

TEST_CASE("id: prefix is only special when it is the entire path (single segment)", "[uri][parse][id_edge]") {
	// "gdrive://id:1a2b3c/more" -- decision: id: is recognized only when it is
	// the ENTIRE remaining path (no further slash-separated segments), since
	// a file id addresses a single file, not a traversable location. A slash
	// after "id:..." makes it an ordinary path whose first segment happens to
	// start with "id:".
	auto r = ParseGDriveUri("gdrive://id:1a2b3c/more");
	REQUIRE(r.ok);
	REQUIRE(r.uri.kind == GDriveUriKind::PATH);
	REQUIRE(r.uri.segments == std::vector<std::string>{"id:1a2b3c", "more"});
}

// ---------------------------------------------------------------------------
// ToString round-trips
// ---------------------------------------------------------------------------

TEST_CASE("ToString round-trips the path form", "[uri][tostring]") {
	auto r = ParseGDriveUri("gdrive://Finance/2026/actuals.parquet");
	REQUIRE(r.ok);
	REQUIRE(r.uri.ToString() == "gdrive://Finance/2026/actuals.parquet");
}

TEST_CASE("ToString round-trips the root", "[uri][tostring]") {
	auto r = ParseGDriveUri("gdrive://");
	REQUIRE(r.ok);
	REQUIRE(r.uri.ToString() == "gdrive://");
}

TEST_CASE("ToString round-trips the file id form", "[uri][tostring]") {
	auto r = ParseGDriveUri("gdrive://id:1a2b3c");
	REQUIRE(r.ok);
	REQUIRE(r.uri.ToString() == "gdrive://id:1a2b3c");
}

TEST_CASE("ToString normalizes collapsed slashes and dropped trailing slash", "[uri][tostring]") {
	auto r = ParseGDriveUri("gdrive://a//b/");
	REQUIRE(r.ok);
	REQUIRE(r.uri.ToString() == "gdrive://a/b");
}

// ---------------------------------------------------------------------------
// ParentPath / FileName
// ---------------------------------------------------------------------------

TEST_CASE("ParentPath and FileName on a multi-segment path", "[uri][parent_filename]") {
	auto r = ParseGDriveUri("gdrive://Finance/2026/actuals.parquet");
	REQUIRE(r.ok);
	REQUIRE(r.uri.ParentPath() == "gdrive://Finance/2026");
	REQUIRE(r.uri.FileName() == "actuals.parquet");
}

TEST_CASE("ParentPath and FileName on a single-segment path", "[uri][parent_filename]") {
	auto r = ParseGDriveUri("gdrive://actuals.parquet");
	REQUIRE(r.ok);
	REQUIRE(r.uri.ParentPath() == "gdrive://");
	REQUIRE(r.uri.FileName() == "actuals.parquet");
}

TEST_CASE("ParentPath and FileName on the root", "[uri][parent_filename]") {
	auto r = ParseGDriveUri("gdrive://");
	REQUIRE(r.ok);
	REQUIRE(r.uri.ParentPath() == "gdrive://");
	REQUIRE(r.uri.FileName().empty());
}
