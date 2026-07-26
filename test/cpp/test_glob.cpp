// S-2.18 -- glob pattern matching over Drive file names. Pure logic, no DuckDB linkage.
//
// Drive's API cannot glob: `files.list` supports only exact-name and
// `contains` predicates, so patterns are matched locally against a listing.
// A matcher that is too eager silently widens a scan; one that is too strict
// silently drops files. Hence the exhaustive edge-case coverage here.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "gdrive_glob.hpp"

using duckdb::gdrive::ExpandBraces;
using duckdb::gdrive::GlobSplit;
using duckdb::gdrive::HasGlobMetacharacters;
using duckdb::gdrive::MatchPath;
using duckdb::gdrive::MatchSegment;
using duckdb::gdrive::SplitGlob;

// ---------------------------------------------------------------------------
// MatchSegment -- '*', '?', literal matching within one segment
// ---------------------------------------------------------------------------

TEST_CASE("MatchSegment: literal patterns match only the exact name", "[glob][match_segment]") {
	REQUIRE(MatchSegment("actuals.parquet", "actuals.parquet"));
	REQUIRE_FALSE(MatchSegment("actuals.parquet", "actuals2.parquet"));
	REQUIRE_FALSE(MatchSegment("actuals.parquet", "ACTUALS.PARQUET")); // case-sensitive
}

TEST_CASE("MatchSegment: '*' matches any run of characters, including empty", "[glob][match_segment]") {
	REQUIRE(MatchSegment("*.parquet", "part-000.parquet"));
	REQUIRE(MatchSegment("*.parquet", ".parquet")); // '*' matches empty string
	REQUIRE(MatchSegment("part-*.parquet", "part-.parquet"));
	REQUIRE(MatchSegment("*", ""));
	REQUIRE(MatchSegment("*", "anything"));
	REQUIRE(MatchSegment("a*b*c", "abc"));
	REQUIRE(MatchSegment("a*b*c", "aXXbYYc"));
	REQUIRE_FALSE(MatchSegment("a*b*c", "aXXbYY"));
}

TEST_CASE("MatchSegment: '?' requires exactly one character", "[glob][match_segment]") {
	REQUIRE(MatchSegment("part-?.parquet", "part-1.parquet"));
	REQUIRE_FALSE(MatchSegment("part-?.parquet", "part-.parquet"));
	REQUIRE_FALSE(MatchSegment("part-?.parquet", "part-12.parquet"));
	REQUIRE_FALSE(MatchSegment("?", ""));
	REQUIRE(MatchSegment("?", "x"));
}

TEST_CASE("MatchSegment: leading-dot names and metacharacter-only patterns", "[glob][match_segment]") {
	REQUIRE(MatchSegment("*", ".hidden"));
	REQUIRE(MatchSegment(".*", ".hidden"));
	REQUIRE(MatchSegment("*", "***")); // literal name containing metacharacters, matched by '*'
	REQUIRE(MatchSegment("?", "*"));   // '?' matches a literal '*' character in a name
}

TEST_CASE("MatchSegment: empty pattern only matches empty name", "[glob][match_segment]") {
	REQUIRE(MatchSegment("", ""));
	REQUIRE_FALSE(MatchSegment("", "x"));
}

// ---------------------------------------------------------------------------
// MatchSegment -- character classes
// ---------------------------------------------------------------------------

TEST_CASE("MatchSegment: character class matches any listed character", "[glob][match_segment][class]") {
	REQUIRE(MatchSegment("part-[abc].parquet", "part-a.parquet"));
	REQUIRE(MatchSegment("part-[abc].parquet", "part-b.parquet"));
	REQUIRE(MatchSegment("part-[abc].parquet", "part-c.parquet"));
	REQUIRE_FALSE(MatchSegment("part-[abc].parquet", "part-d.parquet"));
}

TEST_CASE("MatchSegment: character class ranges", "[glob][match_segment][class]") {
	REQUIRE(MatchSegment("part-[a-z].parquet", "part-m.parquet"));
	REQUIRE_FALSE(MatchSegment("part-[a-z].parquet", "part-M.parquet"));
	REQUIRE(MatchSegment("part-[0-9].parquet", "part-5.parquet"));
	REQUIRE_FALSE(MatchSegment("part-[0-9].parquet", "part-x.parquet"));
}

TEST_CASE("MatchSegment: negated classes with both '!' and '^'", "[glob][match_segment][class]") {
	REQUIRE(MatchSegment("part-[!abc].parquet", "part-d.parquet"));
	REQUIRE_FALSE(MatchSegment("part-[!abc].parquet", "part-a.parquet"));
	REQUIRE(MatchSegment("part-[^abc].parquet", "part-d.parquet"));
	REQUIRE_FALSE(MatchSegment("part-[^abc].parquet", "part-b.parquet"));
}

TEST_CASE("MatchSegment: literal ']' as first class member", "[glob][match_segment][class]") {
	// classic glob rule: ']' immediately after '[' or '[!' is a literal
	// member, not the closing bracket.
	REQUIRE(MatchSegment("[]a].txt", "].txt"));
	REQUIRE(MatchSegment("[]a].txt", "a.txt"));
	REQUIRE_FALSE(MatchSegment("[]a].txt", "b.txt"));
	REQUIRE(MatchSegment("[!]a].txt", "b.txt"));
	REQUIRE_FALSE(MatchSegment("[!]a].txt", "].txt"));
}

TEST_CASE("MatchSegment: literal '-' at either end of a class", "[glob][match_segment][class]") {
	REQUIRE(MatchSegment("[a-].txt", "a.txt"));
	REQUIRE(MatchSegment("[a-].txt", "-.txt"));
	REQUIRE_FALSE(MatchSegment("[a-].txt", "b.txt"));
	REQUIRE(MatchSegment("[-a].txt", "a.txt"));
	REQUIRE(MatchSegment("[-a].txt", "-.txt"));
	REQUIRE_FALSE(MatchSegment("[-a].txt", "b.txt"));
}

TEST_CASE("MatchSegment: unclosed '[' is treated as a literal character", "[glob][match_segment][class]") {
	// Decision: an unclosed class is not an error and does not reject the
	// whole match -- '[' degrades to matching itself literally. This keeps
	// the matcher total (never throws) which matters because it runs over
	// untrusted Drive file names, and a thrown exception here would abort
	// an entire folder scan for one oddly-named file.
	REQUIRE(MatchSegment("part-[abc.parquet", "part-[abc.parquet"));
	REQUIRE_FALSE(MatchSegment("part-[abc.parquet", "part-a.parquet"));
	REQUIRE(MatchSegment("[", "["));
	REQUIRE(MatchSegment("[a-", "[a-"));
}

// ---------------------------------------------------------------------------
// MatchPath -- '**' and segment boundaries
// ---------------------------------------------------------------------------

TEST_CASE("MatchPath: '*' does not cross a '/' boundary", "[glob][match_path]") {
	REQUIRE(MatchPath("a/*/c", "a/b/c"));
	REQUIRE_FALSE(MatchPath("a/*/c", "a/b/x/c"));
	REQUIRE_FALSE(MatchPath("a*c", "a/b/c"));
}

TEST_CASE("MatchPath: '**' crosses segment boundaries and matches zero segments", "[glob][match_path]") {
	REQUIRE(MatchPath("a/**/b", "a/b"));           // '**' matches zero segments
	REQUIRE(MatchPath("a/**/b", "a/x/b"));          // one segment
	REQUIRE(MatchPath("a/**/b", "a/x/y/z/b"));      // many segments
	REQUIRE_FALSE(MatchPath("a/**/b", "a/x/y/z/c")); // wrong final segment
	REQUIRE(MatchPath("**", "anything/at/all"));
	REQUIRE(MatchPath("**", ""));
	REQUIRE(MatchPath("**/x.parquet", "x.parquet"));
	REQUIRE(MatchPath("**/x.parquet", "a/b/x.parquet"));
}

TEST_CASE("MatchPath: '**' combined with segment globs", "[glob][match_path]") {
	REQUIRE(MatchPath("fixtures/**/*.parquet", "fixtures/parts/part-000.parquet"));
	REQUIRE(MatchPath("fixtures/**/*.parquet", "fixtures/a/b/c/part-000.parquet"));
	REQUIRE(MatchPath("fixtures/**/*.parquet", "fixtures/part-000.parquet"));
	REQUIRE_FALSE(MatchPath("fixtures/**/*.parquet", "fixtures/part-000.csv"));
}

TEST_CASE("MatchPath: plain literal path with no metacharacters", "[glob][match_path]") {
	REQUIRE(MatchPath("a/b/c.parquet", "a/b/c.parquet"));
	REQUIRE_FALSE(MatchPath("a/b/c.parquet", "a/b/d.parquet"));
}

// ---------------------------------------------------------------------------
// HasGlobMetacharacters
// ---------------------------------------------------------------------------

TEST_CASE("HasGlobMetacharacters: detects each supported metacharacter", "[glob][has_metacharacters]") {
	REQUIRE(HasGlobMetacharacters("*.parquet"));
	REQUIRE(HasGlobMetacharacters("part-?.parquet"));
	REQUIRE(HasGlobMetacharacters("part-[abc].parquet"));
	REQUIRE(HasGlobMetacharacters("a/**/b"));
	REQUIRE(HasGlobMetacharacters("{a,b}.parquet"));
}

TEST_CASE("HasGlobMetacharacters: false for plain names", "[glob][has_metacharacters]") {
	REQUIRE_FALSE(HasGlobMetacharacters("Finance/2026/actuals.parquet"));
	REQUIRE_FALSE(HasGlobMetacharacters(""));
	REQUIRE_FALSE(HasGlobMetacharacters("plain-name_v2.csv"));
}

// ---------------------------------------------------------------------------
// SplitGlob
// ---------------------------------------------------------------------------

TEST_CASE("SplitGlob: no metacharacters at all", "[glob][split]") {
	auto split = SplitGlob("Finance/2026/actuals.parquet");
	REQUIRE(split.literal_prefix == "Finance/2026/actuals.parquet");
	REQUIRE(split.pattern_tail.empty());
	REQUIRE_FALSE(split.recursive);
}

TEST_CASE("SplitGlob: metacharacter in the first segment", "[glob][split]") {
	auto split = SplitGlob("*.parquet");
	REQUIRE(split.literal_prefix.empty());
	REQUIRE(split.pattern_tail == "*.parquet");
	REQUIRE_FALSE(split.recursive);
}

TEST_CASE("SplitGlob: literal directory prefix before a globbed tail", "[glob][split]") {
	auto split = SplitGlob("a/b/part-*.parquet");
	REQUIRE(split.literal_prefix == "a/b");
	REQUIRE(split.pattern_tail == "part-*.parquet");
	REQUIRE_FALSE(split.recursive);
}

TEST_CASE("SplitGlob: '**' anywhere forces recursive", "[glob][split]") {
	auto split = SplitGlob("a/**/b.parquet");
	REQUIRE(split.literal_prefix == "a");
	REQUIRE(split.pattern_tail == "**/b.parquet");
	REQUIRE(split.recursive);
}

TEST_CASE("SplitGlob: multi-segment tail without '**' is still recursive", "[glob][split]") {
	auto split = SplitGlob("a/b-*/c/d.parquet");
	REQUIRE(split.literal_prefix == "a");
	REQUIRE(split.pattern_tail == "b-*/c/d.parquet");
	REQUIRE(split.recursive);
}

TEST_CASE("SplitGlob: a pattern that is only '*'", "[glob][split]") {
	auto split = SplitGlob("*");
	REQUIRE(split.literal_prefix.empty());
	REQUIRE(split.pattern_tail == "*");
	REQUIRE_FALSE(split.recursive);
}

// ---------------------------------------------------------------------------
// ExpandBraces
// ---------------------------------------------------------------------------

TEST_CASE("ExpandBraces: no braces returns the input unchanged", "[glob][braces]") {
	auto result = ExpandBraces("part-*.parquet");
	REQUIRE(result.size() == 1);
	REQUIRE(result[0] == "part-*.parquet");
}

TEST_CASE("ExpandBraces: simple alternation", "[glob][braces]") {
	auto result = ExpandBraces("file.{csv,tsv}");
	REQUIRE(result.size() == 2);
	REQUIRE(result[0] == "file.csv");
	REQUIRE(result[1] == "file.tsv");
}

TEST_CASE("ExpandBraces: nested alternation", "[glob][braces]") {
	auto result = ExpandBraces("file.{a,{b,c}}");
	REQUIRE(result.size() == 3);
	REQUIRE(result[0] == "file.a");
	REQUIRE(result[1] == "file.b");
	REQUIRE(result[2] == "file.c");
}

TEST_CASE("ExpandBraces: empty alternative", "[glob][braces]") {
	auto result = ExpandBraces("file{.bak,}");
	REQUIRE(result.size() == 2);
	REQUIRE(result[0] == "file.bak");
	REQUIRE(result[1] == "file");
}

TEST_CASE("ExpandBraces: multiple brace groups form a cartesian product", "[glob][braces]") {
	auto result = ExpandBraces("{a,b}-{1,2}.csv");
	REQUIRE(result.size() == 4);
	REQUIRE(result[0] == "a-1.csv");
	REQUIRE(result[1] == "a-2.csv");
	REQUIRE(result[2] == "b-1.csv");
	REQUIRE(result[3] == "b-2.csv");
}

TEST_CASE("ExpandBraces: unmatched '{' is left literal", "[glob][braces]") {
	auto result = ExpandBraces("file{a,b.csv");
	REQUIRE(result.size() == 1);
	REQUIRE(result[0] == "file{a,b.csv");
}

TEST_CASE("ExpandBraces: unmatched '}' is left literal", "[glob][braces]") {
	auto result = ExpandBraces("file}a,b.csv");
	REQUIRE(result.size() == 1);
	REQUIRE(result[0] == "file}a,b.csv");
}

// ---------------------------------------------------------------------------
// ExpandBraces -- DoS hardening (codex review 2026-07-26, wave 0)
//
// Signalling convention under test: a normal expansion always returns at
// least one element (even a brace-free literal yields {pattern}), so an
// EMPTY vector is repurposed to signal "rejected: too large to expand
// safely" and is never a legitimate "zero matches" result.
// ---------------------------------------------------------------------------

TEST_CASE("ExpandBraces: an overlong pattern is rejected before any expansion", "[glob][braces][dos]") {
	std::string pattern(5000, 'a'); // no braces at all, just long
	auto start = std::chrono::steady_clock::now();
	auto result = ExpandBraces(pattern);
	auto elapsed = std::chrono::steady_clock::now() - start;

	REQUIRE(result.empty());
	REQUIRE(elapsed < std::chrono::seconds(1));
}

TEST_CASE("ExpandBraces: repeated alternation groups are capped, not exponentially expanded", "[glob][braces][dos]") {
	// "{a,b}" x 20 denotes 2^20 (~1M) concrete patterns if expanded in full.
	std::string pattern;
	for (int i = 0; i < 20; i++) {
		pattern += "{a,b}";
	}
	REQUIRE(pattern.size() < 4096); // exercises the count cap, not the length cap

	auto start = std::chrono::steady_clock::now();
	auto result = ExpandBraces(pattern);
	auto elapsed = std::chrono::steady_clock::now() - start;

	// Rejected outright (empty), not silently truncated to some arbitrary
	// prefix of the 2^20 possible results -- a silent truncation would drop
	// real files from a glob's result set, which is worse than a clear error.
	REQUIRE(result.empty());
	REQUIRE(elapsed < std::chrono::seconds(1));
}

TEST_CASE("ExpandBraces: deeply nested (but not combinatorially explosive) braces are also capped",
          "[glob][braces][dos]") {
	// 200 levels of nesting: "{{{...{a}...}}}" -- very few final strings, but
	// the recursion-depth cap must catch this independently of the
	// expansion-count cap, or a deep-enough nesting could exhaust the stack.
	std::string pattern = "a";
	for (int i = 0; i < 200; i++) {
		pattern = "{" + pattern + "}";
	}

	auto result = ExpandBraces(pattern);
	REQUIRE(result.empty());
}

TEST_CASE("ExpandBraces: a pattern just under the caps still expands normally", "[glob][braces][dos]") {
	// Sanity check that the caps do not clip ordinary, reasonably-sized
	// patterns -- 4 groups of 2 alternatives is 16 combinations, nowhere
	// near the 1024 budget.
	auto result = ExpandBraces("{a,b}-{1,2}-{x,y}-{p,q}.csv");
	REQUIRE(result.size() == 16);
}

// ---------------------------------------------------------------------------
// UTF-8: byte-wise matching decision
// ---------------------------------------------------------------------------

TEST_CASE("MatchSegment: UTF-8 names match byte-wise under '*'", "[glob][utf8]") {
	// "grüße.csv" -- multi-byte codepoints throughout ('ü' and 'ß' are each
	// 2 bytes in UTF-8). '*' is byte-agnostic: it happily spans multi-byte
	// sequences because it does not interpret them at all. Note: this
	// source file is UTF-8, so these string literals are the raw UTF-8
	// bytes, not escape sequences -- hex escapes (`\xC3\xBC`) are avoided
	// here deliberately, because `\x` greedily consumes any hex digit
	// (including 'a'-'f') that immediately follows, silently swallowing
	// adjacent characters.
	REQUIRE(MatchSegment("gr*.csv", "grüße.csv"));
	REQUIRE(MatchSegment("*.csv", "grüße.csv"));
}

TEST_CASE("MatchSegment: '?' matches exactly one byte, not one codepoint", "[glob][utf8]") {
	// Decision: this matcher is byte-wise throughout, not codepoint-aware.
	// 'ü' is 2 bytes (0xC3 0xBC) in UTF-8, so a single '?' cannot stand in
	// for it -- this is the sharp edge the task calls out explicitly.
	REQUIRE_FALSE(MatchSegment("gr?e.csv", "grüe.csv"));
	REQUIRE(MatchSegment("gr??e.csv", "grüe.csv")); // two '?' for two bytes
	REQUIRE(MatchSegment("gr?e.csv", "grXe.csv"));  // one ASCII byte, one '?'
}

// ---------------------------------------------------------------------------
// Catastrophic backtracking
// ---------------------------------------------------------------------------

TEST_CASE("MatchSegment: pathological multi-star pattern completes quickly", "[glob][backtracking]") {
	// A naive recursive matcher is exponential on `*a*a*a*a*a*b` against a
	// long string with no 'b' at all. The standard two-pointer backtracking
	// algorithm is linear-ish (bounded, no exponential blowup) here.
	std::string name(10000, 'a');
	std::string pattern = "*a*a*a*a*a*b";

	auto start = std::chrono::steady_clock::now();
	bool matched = MatchSegment(pattern, name);
	auto elapsed = std::chrono::steady_clock::now() - start;

	REQUIRE_FALSE(matched); // no 'b' anywhere in `name`
	REQUIRE(elapsed < std::chrono::seconds(1));
}

TEST_CASE("MatchPath: pathological '**' pattern completes quickly", "[glob][backtracking]") {
	std::string pattern = "**/**/**/**/**/**/**/**/**/**/x";
	std::string path;
	for (int i = 0; i < 500; i++) {
		path += "seg" + std::to_string(i) + "/";
	}
	path += "y"; // never matches the required trailing "x"

	auto start = std::chrono::steady_clock::now();
	bool matched = MatchPath(pattern, path);
	auto elapsed = std::chrono::steady_clock::now() - start;

	REQUIRE_FALSE(matched);
	REQUIRE(elapsed < std::chrono::seconds(1));
}
