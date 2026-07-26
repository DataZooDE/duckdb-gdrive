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

} // namespace gdrive
} // namespace duckdb
