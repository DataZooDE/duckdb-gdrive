// S-2.1 -- gdrive:// URI parsing. Pure logic, no DuckDB linkage.
#include <catch2/catch_test_macros.hpp>

#include "gdrive_uri.hpp"

using duckdb::gdrive::GDriveUri;
using duckdb::gdrive::GDriveUriKind;
using duckdb::gdrive::GDriveUriParse;
using duckdb::gdrive::IsFileIdFallbackGlobProbe;
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

TEST_CASE("id: prefix followed by a path segment is rejected, not silently reinterpreted", "[uri][parse][id_edge]") {
	// "gdrive://id:1a2b3c/more" -- decision (codex review 2026-07-26, wave 0):
	// a leading "id:" addresses a single file, not a traversable location, so
	// the URI must be exactly "gdrive://id:<id>" with no further segments.
	// Silently reinterpreting this as an ordinary path whose first segment
	// happens to be "id:1a2b3c" would mask what is almost certainly a typo,
	// and could silently resolve a real folder that happens to be named
	// "id:1a2b3c" -- a worse outcome than a clear, immediate error.
	auto r = ParseGDriveUri("gdrive://id:1a2b3c/more");
	REQUIRE_FALSE(r.ok);
	CAPTURE(r.error);
	REQUIRE(r.error == "the gdrive://id: form addresses a single file and cannot have path segments");
}

TEST_CASE("id: prefix with a trailing slash and nothing else is also rejected", "[uri][parse][id_edge]") {
	auto r = ParseGDriveUri("gdrive://id:1a2b3c/");
	REQUIRE_FALSE(r.ok);
	CAPTURE(r.error);
	REQUIRE(r.error == "the gdrive://id: form addresses a single file and cannot have path segments");
}

TEST_CASE("id: prefix followed by multiple path segments is rejected", "[uri][parse][id_edge]") {
	auto r = ParseGDriveUri("gdrive://id:1a2b3c/more/still-more.csv");
	REQUIRE_FALSE(r.ok);
	CAPTURE(r.error);
	REQUIRE(r.error == "the gdrive://id: form addresses a single file and cannot have path segments");
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

// ---------------------------------------------------------------------------
// IsFileIdFallbackGlobProbe -- the narrow carve-out Glob() uses to recognise
// DuckDB core's FALLBACK_GLOB directory-probe retry (bug found on the first
// live run against real Drive, 2026-07-26): a nonexistent gdrive://id:<x>
// resolves to zero matches, so core retries as if it were a directory by
// appending "/**/*.<ext>" (FileSystem::GlobFileList, file_system.cpp). That
// synthesized path fails ParseGDriveUri's deliberate id:+segments rejection
// (M-5) even though no user ever typed it. This helper must match ONLY that
// exact synthesized shape -- anything else (in particular a genuine user
// typo) must keep surfacing M-5's rejection unchanged.
// ---------------------------------------------------------------------------

TEST_CASE("IsFileIdFallbackGlobProbe recognises the exact core-synthesized shape",
          "[uri][fallback_glob_probe]") {
	REQUIRE(IsFileIdFallbackGlobProbe("gdrive://id:1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/**/*.csv"));
	REQUIRE(IsFileIdFallbackGlobProbe("gdrive://id:abc/**/*.parquet"));
	REQUIRE(IsFileIdFallbackGlobProbe("gdrive://id:abc/**/*.json"));
	// core's JoinPath uses whatever extension FileGlobInput carries -- more
	// than three letters, dots aside, must still match.
	REQUIRE(IsFileIdFallbackGlobProbe("gdrive://id:abc/**/*.tar.gz"));
}

TEST_CASE("IsFileIdFallbackGlobProbe rejects a genuine user typo with one extra segment",
          "[uri][fallback_glob_probe]") {
	// This is exactly M-5's target case: a user wrote this by hand. It must
	// NOT be recognised as the fallback-probe shape, so Glob() keeps
	// surfacing ParseGDriveUri's rejection for it.
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:abc/oops"));
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:1a2b3c/more"));
}

TEST_CASE("IsFileIdFallbackGlobProbe rejects near misses of the synthesized shape",
          "[uri][fallback_glob_probe]") {
	// First segment must be the literal "**", not any other glob text.
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:abc/*/*.csv"));
	// Second segment must start with "*.", not be a bare wildcard or a
	// literal name.
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:abc/**/*"));
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:abc/**/file.csv"));
	// No third, deeper segment.
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:abc/**/*.csv/extra"));
	// Missing the "**" segment entirely (just one segment).
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:abc/*.csv"));
	// Empty file id.
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:/**/*.csv"));
}

TEST_CASE("IsFileIdFallbackGlobProbe rejects non-id: URIs and non-gdrive strings",
          "[uri][fallback_glob_probe]") {
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://Finance/**/*.csv"));
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("gdrive://id:abc")); // no segments at all
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("s3://id:abc/**/*.csv"));
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe(""));
}
