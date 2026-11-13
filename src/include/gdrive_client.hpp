#pragma once

#include "gdrive_errors.hpp"
#include "gdrive_upload_plan.hpp"

#include <cstddef>
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
	//! In the trash. Listings exclude trashed files in their query; an id:
	//! path does not, and a trashed id must read as gone, not as a file.
	bool trashed = false;

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
	//! files.generateIds: id reservations that make creates retry-safe. Not a
	//! files_get, though it is a GET.
	int64_t generate_ids = 0;
	int64_t cache_hits = 0;
	int64_t cache_misses = 0;
	int64_t retries = 0;
	//! 401s that triggered a refresh attempt -- at most one per logical call.
	//! Counted BEFORE knowing whether a new token can be had: a pasted
	//! ACCESS_TOKEN alone has nothing to refresh with (its 401 stands, with no
	//! re-send), and a refresh whose mint fails still counts.
	int64_t auth_refresh = 0;
	//! Token mint attempts, successful or not: each call into a provider's
	//! mint (refresh grant, RFC 7523, STS + impersonation, browser flow). A
	//! failure that never reached an endpoint counts too. A mint skipped
	//! because the same credential failed moments ago does not.
	int64_t token_request = 0;
	//! Chunk PUTs carrying data to a resumable upload session (streaming
	//! writes), every attempt. The session's initiation is counted as
	//! files_create / files_update.
	int64_t upload_chunks = 0;
	//! Empty status-query PUTs ("bytes */*") a resumable upload sends before
	//! resuming after an error, counted apart from data chunks.
	int64_t upload_status_queries = 0;
	//! Resumable sessions cancelled: an error mid-stream, or a write handle
	//! destroyed without a successful Close(). Nothing was published.
	int64_t upload_aborts = 0;
	//! HIGH-WATER MARK, not a count: the most bytes one write handle held in
	//! memory (bounded by gdrive_upload_chunk_bytes). Not in Total().
	int64_t upload_buffer_peak_bytes = 0;
	//! The metadata endpoints behind the table functions: permissions.list,
	//! revisions.list (and revisions.get), changes.list (and
	//! getStartPageToken), drives.list.
	int64_t permissions_list = 0;
	int64_t revisions_list = 0;
	//! Ranged downloads of an EARLIER version (`?version=`), apart from
	//! files_media so a cost assertion on current-version reads stays exact.
	int64_t revisions_media = 0;
	int64_t changes_list = 0;
	int64_t drives_list = 0;

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
	//! The Location header, when Drive sent one (a resumable session URI).
	std::string location;
	//! An attempt before the final one went unanswered (transport failure or
	//! 5xx) and was re-sent, so it may have been applied. What makes a later
	//! 409 fileIdInUse "ours" (InterpretCreateReply).
	bool had_ambiguous_attempt = false;
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
	//!
	//! `file_fields`, when non-empty, replaces the narrow field mask inside
	//! `files(...)` -- the table functions ask for owners, links, checksums.
	//! The combined body keeps every field Drive returned.
	virtual DriveResponse List(const std::string &query, const std::string &page_token = "",
	                           const std::string &file_fields = "") = 0;

	//! One GET against the Drive API v3, retried and counted like every other
	//! call. `path_and_query` starts at "/drive/v3/"; the caller adds
	//! AllDrivesQuery() where the endpoint takes it (drives.list does not).
	//! Does not follow pages: the callers each have their own token field.
	virtual DriveResponse ApiGet(const std::string &path_and_query, int64_t DriveCallStats::*counter) = 0;

	//! files.get?alt=media with `Range: bytes=start-end` (inclusive, as HTTP
	//! defines it). `end` < 0 means "to end of file".
	virtual DriveResponse Download(const std::string &file_id, int64_t start, int64_t end) = 0;

	//! revisions.get?alt=media: the same ranged read of an EARLIER version
	//! (gdrive://...?version=<id>), counted as revisions_media.
	virtual DriveResponse DownloadRevision(const std::string &file_id, const std::string &revision_id, int64_t start,
	                                       int64_t end) = 0;

	//! files.export. Does NOT support Range -- the whole export is fetched.
	virtual DriveResponse Export(const std::string &file_id, const std::string &mime_type) = 0;

	//! Resumable upload; creates when `file_id` is empty, updates otherwise.
	//! Reserve a file id before creating anything (files.generateIds).
	//!
	//! This is what makes a CREATE retryable. A transport failure is
	//! ambiguous -- the request may have been applied and only the response
	//! lost -- so a blind retry of `files.create` can produce a second file
	//! with the same name, which R-4 then reports as an unaddressable path
	//! forever. With the id chosen up front, a retry either succeeds or comes
	//! back 409 "A file already exists with the provided ID", which proves
	//! the first attempt landed. Verified against the live API.
	//!
	//! Costs one round trip per create -- or per CHAIN: a write into a new
	//! subtree reserves every missing folder and the file in one call.
	//! Counted as generate_ids. A create that cannot reserve an id is
	//! refused (fail closed), never sent unreserved.
	virtual DriveResponse GenerateFileIds(int count) = 0;

	//! Creates when `file_id` is empty, overwrites its content otherwise. A
	//! create must carry `reserved_id`; with one, it is retried freely and its
	//! final reply is interpreted by InterpretCreateReply: a 409 fileIdInUse
	//! after an unanswered attempt (STRICT: never without one -- that is a
	//! loud error naming the id), or no usable answer at all, is resolved by
	//! looking the id up, so the
	//! caller sees either the created object or an error that means nothing
	//! was created (or, when even that lookup fails, that re-running is safe).
	//! An overwrite is retried too: the same bytes on the same id.
	virtual DriveResponse Upload(const std::string &file_id, const std::string &parent_id,
	                             const std::string &name, const std::string &content_type,
	                             const std::string &data,
	                             const std::string &reserved_id = "") = 0;

	//! STREAMING writes over a Drive resumable upload session.
	//!
	//! StartResumableUpload initiates a session (files.create when `file_id`
	//! is empty, files.update otherwise) and returns its URI in `location`.
	//! It creates NO file: Drive publishes the file only when the final chunk
	//! lands, so initiation is safe to retry. UploadChunk PUTs the buffer's
	//! pending chunk ("bytes a-b/*" unless `final`), retrying with backoff
	//! and resuming from the session's own Range report; a final chunk
	//! answers with the file's metadata in `body`. CancelUpload discards the
	//! session (best effort, short timeout). Chunk PUTs carry no bearer token
	//! -- the session URI is the credential -- so once the session is open, a
	//! long upload cannot fail on token expiry. (Initiation itself uses the
	//! token captured at OpenFile.)
	//! A create's session carries `reserved_id`, so its final chunk publishes
	//! under that id and an ambiguous final chunk can be looked up by it.
	virtual DriveResponse StartResumableUpload(const std::string &file_id, const std::string &parent_id,
	                                           const std::string &name, const std::string &content_type,
	                                           const std::string &reserved_id = "") = 0;
	virtual DriveResponse UploadChunk(const std::string &session_uri, const ChunkedUploadBuffer &buf,
	                                  bool final) = 0;
	virtual void CancelUpload(const std::string &session_uri) = 0;

	//! Trash (default) or permanently delete -- see decision D-6.
	virtual DriveResponse Delete(const std::string &file_id, bool permanent) = 0;

	//! files.update changing name and/or parents. Retried after an ambiguous
	//! failure (it sets absolute state); see MutateMoveFile for the
	//! reconcile-by-id when every attempt went unanswered.
	virtual DriveResponse Move(const std::string &file_id, const std::string &new_parent_id,
	                           const std::string &new_name, const std::string &old_parent_id) = 0;

	virtual const DriveCallStats &Stats() const = 0;
	virtual void ResetStats() = 0;
};

//! "supportsAllDrives=true&includeItemsFromAllDrives=true" -- see CLAUDE.md:
//! without it Drive answers "not found" for files in a Shared Drive.
std::string AllDrivesQuery();
//! Percent-encoding for one query-string value.
std::string QueryEncode(const std::string &value);

//! Parse a files.get / files.list JSON payload. PURE enough to unit-test, but
//! kept here because it is meaningless without the transport.
bool ParseFileMeta(const std::string &json_object, DriveFileMeta &out);

//! Every id from a files.generateIds response, in order; false unless there
//! are exactly `count` non-empty ones. See GDriveClient::GenerateFileIds for
//! why a create reserves its id before making the file.
bool ParseGeneratedFileIds(const std::string &json, std::size_t count, std::vector<std::string> &out);
//! `name` and `parents` of a files.get?fields=id,name,parents reply -- what a
//! move is reconciled against (MoveLanded).
bool ParseNameAndParents(const std::string &json, std::string &name, std::vector<std::string> &parents);
bool ParseFileList(const std::string &json_body, std::vector<DriveFileMeta> &out,
                   std::string &next_page_token);

} // namespace gdrive
} // namespace duckdb
