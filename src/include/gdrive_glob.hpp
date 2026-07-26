#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Glob pattern matching over Drive file names. PURE.
//
// Drive's API cannot glob. `files.list` supports only exact-name and
// `contains` predicates, so `gdrive://folder/*.parquet` is served by listing
// the folder and filtering locally. That makes the matcher a pure function
// over strings, and worth getting exactly right: a matcher that is too eager
// silently widens a user's scan, and one that is too strict silently drops
// files from a result set. Both are invisible without tests.
//
// Supported, matching DuckDB's own filesystem glob semantics:
//   *        any run of characters within one path segment (never '/')
//   ?        exactly one character within one segment
//   [abc]    character class, with [a-z] ranges and [!abc] / [^abc] negation
//   **       any number of segments, including zero (recursive descent)
//   {a,b}    alternation
// ---------------------------------------------------------------------------

//! True when `name` (a single path segment, no slashes) matches `pattern`.
//! `pattern` must not contain '/' or '**'.
bool MatchSegment(const std::string &pattern, const std::string &name);

//! True when a full slash-separated `path` matches a full `pattern`, honouring
//! '**' across segment boundaries.
bool MatchPath(const std::string &pattern, const std::string &path);

//! True when the pattern contains any metacharacter. A pattern-free path is
//! resolved directly, which is the difference between one API call and
//! listing an entire folder.
bool HasGlobMetacharacters(const std::string &pattern);

//! Split a pattern into the longest literal prefix and the remaining pattern.
//! The prefix can be resolved via the (cached) path resolver; only the tail
//! needs listing. For "a/b/part-*.parquet" this yields {"a/b", "part-*.parquet"}.
struct GlobSplit {
	std::string literal_prefix;
	std::string pattern_tail;
	//! True when the tail spans multiple segments ('**' or a '/' in the tail),
	//! which forces a recursive listing rather than a single-folder one.
	bool recursive = false;
};

GlobSplit SplitGlob(const std::string &pattern);

//! Expand {a,b}{c,d} alternations into the concrete patterns they denote.
//! Returns the input unchanged when there is no alternation.
std::vector<std::string> ExpandBraces(const std::string &pattern);

} // namespace gdrive
} // namespace duckdb
