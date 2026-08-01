#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// gdrive:// URI parsing. PURE: no duckdb.hpp, no I/O, so the Catch2 binary
// compiles it directly and there is nothing to mock.
//
// Two addressing forms exist, for the reason set out in HLD section 4:
//
//   gdrive://Finance/2026/actuals.parquet
//       Human-writable path. Drive has NO path addressing, so this costs one
//       files.list per segment to resolve (risk R-1).
//
//   gdrive://id:1a2b3c...
//       Direct file id. Zero resolution calls. This is the form that
//       generated or stored queries should use.
//
// A path may also name a Shared Drive root; that binding lives in the secret,
// not in the URI, so parsing stays free of any notion of "which drive".
// ---------------------------------------------------------------------------

constexpr const char *GDRIVE_SCHEME = "gdrive://";

enum class GDriveUriKind {
	//! gdrive://a/b/c.parquet -- resolve segment by segment
	PATH,
	//! gdrive://id:<fileId> -- already resolved
	FILE_ID,
};

struct GDriveUri {
	GDriveUriKind kind = GDriveUriKind::PATH;
	//! Populated when kind == FILE_ID.
	std::string file_id;
	//! Populated when kind == PATH. Empty vector means the drive root.
	std::vector<std::string> segments;

	//! Round-trips back to the canonical textual form. Used in error messages,
	//! so a user is always shown a path they could paste back.
	std::string ToString() const;
	//! The parent path (all but the last segment). Meaningless for FILE_ID.
	std::string ParentPath() const;
	//! The final segment -- the file name. Empty for the root.
	std::string FileName() const;
};

struct GDriveUriParse {
	bool ok = false;
	//! Human-readable reason, suitable for putting straight into an exception
	//! message. Empty when ok.
	std::string error;
	GDriveUri uri;
};

//! Cheap prefix test used by FileSystem::CanHandleFile. Must not allocate or
//! validate -- DuckDB calls it on every path it sees, for every filesystem.
bool IsGDriveUri(const std::string &uri);

//! Full parse. Never throws: pure sources cannot depend on DuckDB's exception
//! types, so the DuckDB-side caller turns `error` into an exception.
//!
//! Rules:
//!   - scheme must be exactly "gdrive://"
//!   - empty and "." and ".." segments are rejected (".." would imply a
//!     parent traversal Drive has no concept of)
//!   - repeated slashes collapse
//!   - a trailing slash is accepted and ignored
//!   - "id:" with an empty id is an error
//!   - percent-decoding is NOT performed: Drive file names may legitimately
//!     contain '%', and DuckDB hands us the path already un-escaped
GDriveUriParse ParseGDriveUri(const std::string &uri);

//! True iff `uri` has EXACTLY the shape "gdrive://id:<fileId>/**/*.<ext>"
//! (one literal "**" segment, then one "*.<ext>" segment, then nothing
//! else) -- the precise, and ONLY the precise, string DuckDB core's
//! FALLBACK_GLOB directory-probe heuristic synthesizes.
//!
//! FileSystem::GlobFileList (duckdb/src/common/file_system.cpp) reacts to
//! a literal, non-glob path resolving to zero matches by retrying as if
//! that path were a directory: `JoinPath(JoinPath(pattern, "**"), "*." +
//! extension)`. Handed a bare "gdrive://id:<fileId>" whose file id does
//! not exist, that retry produces exactly this shape and hands it back to
//! Glob() -- not a user typo, just core's generic "maybe it's a
//! directory" heuristic, which does not know gdrive's id: form can never
//! be a directory and never has children. The honest answer to "what
//! matches gdrive://id:X/**/*.csv" is always "nothing", which is what
//! Glob() should return instead of surfacing ParseGDriveUri's deliberate
//! "the gdrive://id: form addresses a single file and cannot have path
//! segments" rejection (codex review 2026-07-26, wave 0, finding M-5) for
//! a pattern the user never wrote.
//!
//! Matching MUST be this exact shape, not "any id: URI with a slash in
//! it": a real user typo like "gdrive://id:abc/oops" must keep surfacing
//! M-5's rejection unchanged, in Glob() as everywhere else. Every OTHER
//! caller of ParseGDriveUri (OpenFile, ResolvePath, FileExists, ...) is
//! unaffected by this helper and keeps rejecting all id:+segments shapes.
bool IsFileIdFallbackGlobProbe(const std::string &uri);

//! True if `path` lies at or under one of the comma-separated prefixes in
//! `csv_prefixes`. Used by the `gdrive_immutable_prefixes` setting.
//!
//! Matching is on whole path SEGMENTS, never on characters: the prefix
//! `gdrive://lake` matches `gdrive://lake` and `gdrive://lake/data/x.parquet`
//! but NOT `gdrive://lakehouse/x.parquet`. A plain `starts_with` would match
//! the last one, and since the setting suppresses a freshness check, a
//! neighbouring directory silently inheriting "assume immutable" is a
//! correctness bug, not a cosmetic one.
//!
//! A trailing slash on a prefix is insignificant, entries are trimmed, and
//! empty entries are ignored -- so `"a/, ,b"` is the two prefixes `a` and `b`.
//! An empty `csv_prefixes` matches nothing, which is the default and means the
//! feature is off.
bool PathMatchesAnyPrefix(const std::string &path, const std::string &csv_prefixes);

} // namespace gdrive
} // namespace duckdb
