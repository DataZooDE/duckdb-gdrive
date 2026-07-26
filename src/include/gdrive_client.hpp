#pragma once

#include "gdrive_errors.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

class ClientContext;

namespace gdrive {

// ---------------------------------------------------------------------------
// The Drive API v3 transport. DuckDB-coupled (it uses DuckDB's vendored
// httplib), so everything here is exercised by LIVE tests against real Google
// Drive -- there is no fake and no replay (decision D-1).
//
// Kept behind a narrow interface so the filesystem, resolver and uploader all
// funnel through one place that counts calls, applies retry/backoff, and maps
// errors exactly once.
// ---------------------------------------------------------------------------

//! One file's metadata, as the fields we actually ask Drive for.
struct DriveFileMeta {
	std::string id;
	std::string name;
	std::string mime_type;
	//! Absent for native Google formats -- they have no byte stream at all.
	//! -1 when Drive did not report it.
	int64_t size = -1;
	//! RFC 3339, as Drive returns it.
	std::string modified_time;
	//! Feeds FileSystem::GetVersionTag, which DuckDB's caching layer uses as
	//! the invalidation key. Drive's revision id fits that contract exactly.
	std::string head_revision_id;

	bool IsFolder() const;
	//! application/vnd.google-apps.* -- has no bytes; must be read via
	//! files.export rather than alt=media (REQ-F-07).
	bool IsNativeGoogleFormat() const;
};

//! Per-connection counters. Exposed through gdrive_stats() and asserted on by
//! the live tests: the R-1 mitigation is only real if the amplification factor
//! is measured rather than assumed.
struct DriveCallStats {
	int64_t files_get = 0;
	int64_t files_list = 0;
	int64_t files_media = 0;
	int64_t files_export = 0;
	int64_t files_create = 0;
	int64_t files_update = 0;
	int64_t files_delete = 0;
	int64_t cache_hits = 0;
	int64_t cache_misses = 0;
	int64_t retries = 0;

	int64_t Total() const;
};

//! Result of a transport call. Errors are returned, not thrown, so the caller
//! decides between retry, fallback (export vs media) and surfacing.
struct DriveResponse {
	bool ok = false;
	int http_status = 0;
	std::string body;
	//! Populated when !ok.
	GDriveError error;
};

class GDriveClient {
public:
	virtual ~GDriveClient() = default;

	//! files.get with the metadata field mask.
	virtual DriveResponse GetMetadata(const std::string &file_id) = 0;

	//! files.list. `query` is a Drive `q` expression; the client adds
	//! supportsAllDrives / includeItemsFromAllDrives and the drive scoping.
	//! Follows nextPageToken to exhaustion -- a caller must never see a
	//! partial listing, which is how "the glob silently dropped files" bugs
	//! happen.
	virtual DriveResponse List(const std::string &query, const std::string &page_token = "") = 0;

	//! files.get?alt=media with `Range: bytes=start-end` (inclusive, as HTTP
	//! defines it). `end` < 0 means "to end of file".
	virtual DriveResponse Download(const std::string &file_id, int64_t start, int64_t end) = 0;

	//! files.export. Does NOT support Range -- the whole export is fetched.
	virtual DriveResponse Export(const std::string &file_id, const std::string &mime_type) = 0;

	//! Resumable upload; creates when `file_id` is empty, updates otherwise.
	virtual DriveResponse Upload(const std::string &file_id, const std::string &parent_id,
	                             const std::string &name, const std::string &content_type,
	                             const std::string &data) = 0;

	//! Trash (default) or permanently delete -- see decision D-6.
	virtual DriveResponse Delete(const std::string &file_id, bool permanent) = 0;

	//! files.update changing name and/or parents.
	virtual DriveResponse Move(const std::string &file_id, const std::string &new_parent_id,
	                           const std::string &new_name, const std::string &old_parent_id) = 0;

	virtual const DriveCallStats &Stats() const = 0;
	virtual void ResetStats() = 0;
};

//! Parse a files.get / files.list JSON payload. PURE enough to unit-test, but
//! kept here because it is meaningless without the transport.
bool ParseFileMeta(const std::string &json_object, DriveFileMeta &out);
bool ParseFileList(const std::string &json_body, std::vector<DriveFileMeta> &out,
                   std::string &next_page_token);

} // namespace gdrive
} // namespace duckdb
