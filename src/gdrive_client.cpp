// S-2.2 / S-2.5 / part of S-4.1 -- the Drive API v3 transport.
//
// See src/include/gdrive_client.hpp for the frozen contract and rationale,
// and CLAUDE.md's decision D-1 (real Drive is the
// only integration target, no fake server, no HTTP replay) for why this file
// has no Catch2 coverage of its networking behaviour.
//
// Deliberately has ZERO #include of duckdb.hpp or any duckdb/... header: the
// frozen GDriveClient interface never needs a ClientContext (only forward-
// declares it). That, plus the GDRIVE_CLIENT_PURE_ONLY guard below, is what
// lets test/cpp/test_client.cpp pull this file in directly (via #include of
// this .cpp as a single translation unit -- CMakeLists.txt's PURE_SOURCES
// list is a fixed, explicit list we are not allowed to edit, so this is the
// only way to Catch2-test this file's pure pieces without a build-system
// change) and exercise them -- URL/field-mask/Range building, ParseFileMeta,
// ParseFileList -- with no DuckDB linkage at all.
//
// IMPORTANT: duckdb's vendored httplib.hpp is NOT a drop-in for upstream
// cpp-httplib -- it is implemented in terms of duckdb::InternalException and
// duckdb_re2::Regex, both of which only exist once linked against DuckDB
// (confirmed the hard way: linking it into the Catch2 binary, which links no
// DuckDB code at all, fails with undefined references to both). So the
// networking section below -- everything that #includes httplib.hpp, i.e.
// GDriveClientImpl and its supporting retry/multipart helpers -- is compiled
// out under GDRIVE_CLIENT_PURE_ONLY. test_client.cpp defines that macro
// before including this file; the real extension build (this file compiled
// normally as part of DUCKDB_SOURCES, already listed in CMakeLists.txt) never
// defines it, so it gets everything.
#include "gdrive_client.hpp"
#include "gdrive_errors.hpp"

#include <cstdint>
#include <picojson/picojson.h>

#include <cstdlib>
#include <sstream>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// DriveFileMeta / DriveCallStats -- method bodies for the frozen struct
// declarations in gdrive_client.hpp.
// ---------------------------------------------------------------------------

namespace {
constexpr const char *kNativeGoogleAppsPrefix = "application/vnd.google-apps.";
constexpr const char *kFolderMimeType = "application/vnd.google-apps.folder";
} // namespace

bool DriveFileMeta::IsFolder() const {
	return mime_type == kFolderMimeType;
}

bool DriveFileMeta::IsNativeGoogleFormat() const {
	// Every vnd.google-apps.* type has no byte stream EXCEPT the folder type,
	// which is not a "format" with an export mapping at all -- it is handled
	// via IsFolder()/directory operations, never via files.export.
	if (mime_type.rfind(kNativeGoogleAppsPrefix, 0) != 0) {
		return false;
	}
	return mime_type != kFolderMimeType;
}

int64_t DriveCallStats::Total() const {
	// Deliberately excludes cache_hits/cache_misses (not Drive API calls) and
	// `retries` (a retry is already counted once in whichever kind counter it
	// belongs to -- see GDriveClientImpl::ExecuteWithRetry, which increments
	// the kind counter on every attempt including retries, so Total() below
	// already reflects the true round-trip count Google would see, which is
	// exactly what the R-1 amplification measurement needs).
	return files_get + files_list + files_media + files_export + files_create + files_update + files_delete;
}

// ---------------------------------------------------------------------------
// Pure helpers -- URL encoding, field masks, Range header, JSON parsing.
// Not in an anonymous namespace: test/cpp/test_client.cpp includes this file
// directly (see file header comment) and calls these by name, tagged [client].
// ---------------------------------------------------------------------------

namespace internal {

std::string UrlEncode(const std::string &value) {
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (unsigned char c : value) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(hex[(c >> 4) & 0xF]);
			out.push_back(hex[c & 0xF]);
		}
	}
	return out;
}

//! The exact field mask REQ-NF-01 requires for a single file resource: no
//! more, no less than what the extension actually consumes.
std::string FileFieldsMask() {
	return "id,name,mimeType,size,modifiedTime,headRevisionId";
}

//! Same fields, nested under a files.list response's `files` array, plus
//! nextPageToken so pagination can be followed.
std::string ListFieldsMask() {
	return "nextPageToken,files(" + FileFieldsMask() + ")";
}

//! `Range: bytes=start-end`, inclusive per HTTP semantics. end < 0 means "to
//! end of file" -- omit the end value entirely rather than writing a
//! sentinel, which is what "bytes=start-" means to a real HTTP server.
std::string BuildRangeHeader(int64_t start, int64_t end) {
	std::ostringstream oss;
	oss << "bytes=" << start << "-";
	if (end >= 0) {
		oss << end;
	}
	return oss.str();
}

//! `supportsAllDrives=true&includeItemsFromAllDrives=true` on every single
//! call -- requirement 1. `includeItemsFromAllDrives` is documented by Google
//! as meaningful only for files.list, but sending it unconditionally (rather
//! than threading a per-call-kind exception through every call site) is
//! simpler and Drive ignores parameters it does not recognise for a given
//! endpoint; the alternative (a caller forgetting it on the one endpoint that
//! needs it) is the actual failure mode this requirement exists to prevent.
std::string AllDrivesParams() {
	return "supportsAllDrives=true&includeItemsFromAllDrives=true";
}

} // namespace internal

using internal::AllDrivesParams;
using internal::BuildRangeHeader;
using internal::FileFieldsMask;
using internal::ListFieldsMask;
using internal::UrlEncode;

namespace {

//! Best-effort string extraction; absent/wrong-typed fields are left as the
//! caller's default rather than treated as a parse failure -- Drive's field
//! mask means most responses only carry the fields we asked for, but a
//! partial or defensively-parsed response should still yield what it can.
bool GetString(const picojson::object &obj, const char *key, std::string &out) {
	auto it = obj.find(key);
	if (it == obj.end() || !it->second.is<std::string>()) {
		return false;
	}
	out = it->second.get<std::string>();
	return true;
}

//! Drive's `size` field is a string-encoded int64 in the JSON wire format
//! (Google's convention for values that may exceed a JS safe integer), so it
//! must be parsed from a string, not read as a JSON number.
bool GetInt64FromString(const picojson::object &obj, const char *key, int64_t &out) {
	std::string s;
	if (!GetString(obj, key, s) || s.empty()) {
		return false;
	}
	errno = 0;
	char *end = nullptr;
	long long v = std::strtoll(s.c_str(), &end, 10);
	if (end == s.c_str() || errno == ERANGE) {
		return false;
	}
	out = static_cast<int64_t>(v);
	return true;
}

bool ParseFileMetaObject(const picojson::object &obj, DriveFileMeta &out) {
	// `id` is the one field whose absence means "this is not a file resource
	// at all" (an error body, a malformed payload, ...) -- everything else is
	// optional-ish and defaults sensibly.
	if (!GetString(obj, "id", out.id)) {
		return false;
	}
	GetString(obj, "name", out.name);
	GetString(obj, "mimeType", out.mime_type);
	GetString(obj, "modifiedTime", out.modified_time);
	GetString(obj, "headRevisionId", out.head_revision_id);
	int64_t size = -1;
	if (GetInt64FromString(obj, "size", size)) {
		out.size = size;
	} else {
		out.size = -1;
	}
	return true;
}

} // namespace

//! First id out of a files.generateIds response: {"ids":["...", ...]}.
//!
//! Pure, and deliberately placed with the other parsers so test_client.cpp
//! can exercise it without any DuckDB linkage.
bool ParseGeneratedFileId(const std::string &json, std::string &out) {
	picojson::value root;
	if (!picojson::parse(root, json).empty() || !root.is<picojson::object>()) {
		return false;
	}
	const auto &obj = root.get<picojson::object>();
	auto it = obj.find("ids");
	if (it == obj.end() || !it->second.is<picojson::array>()) {
		return false;
	}
	const auto &arr = it->second.get<picojson::array>();
	if (arr.empty() || !arr[0].is<std::string>()) {
		return false;
	}
	out = arr[0].get<std::string>();
	return !out.empty();
}

bool ParseFileMeta(const std::string &json_object, DriveFileMeta &out) {
	picojson::value root;
	std::string parse_err = picojson::parse(root, json_object);
	if (!parse_err.empty() || !root.is<picojson::object>()) {
		return false;
	}
	return ParseFileMetaObject(root.get<picojson::object>(), out);
}

bool ParseFileList(const std::string &json_body, std::vector<DriveFileMeta> &out, std::string &next_page_token) {
	out.clear();
	next_page_token.clear();

	picojson::value root;
	std::string parse_err = picojson::parse(root, json_body);
	if (!parse_err.empty() || !root.is<picojson::object>()) {
		return false;
	}
	const auto &top = root.get<picojson::object>();

	auto token_it = top.find("nextPageToken");
	if (token_it != top.end() && token_it->second.is<std::string>()) {
		next_page_token = token_it->second.get<std::string>();
	}

	auto files_it = top.find("files");
	if (files_it == top.end()) {
		// No `files` key at all is still a well-formed (empty) listing, not a
		// parse failure -- an empty folder's files.list response omits the key.
		return true;
	}
	if (!files_it->second.is<picojson::array>()) {
		return false;
	}
	const auto &arr = files_it->second.get<picojson::array>();
	out.reserve(arr.size());
	for (const auto &entry : arr) {
		if (!entry.is<picojson::object>()) {
			continue;
		}
		DriveFileMeta meta;
		if (ParseFileMetaObject(entry.get<picojson::object>(), meta)) {
			out.push_back(std::move(meta));
		}
	}
	return true;
}

} // namespace gdrive
} // namespace duckdb

// ---------------------------------------------------------------------------
// Everything below this line does real network I/O (or supports it) and is
// compiled out of the pure-logic test build. See the file header comment for
// why: duckdb's vendored httplib.hpp needs duckdb linked in, which the
// Catch2 binary does not do.
// ---------------------------------------------------------------------------
#ifndef GDRIVE_CLIENT_PURE_ONLY

#include "gdrive_stats.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Process-wide stats aggregate (gdrive_stats.hpp). A function-local static
// behind a mutex -- simplest thing that works for "one DuckDB process talks
// to one Drive account", which is the only case there is today. If a future
// multi-database-instance-per-process scenario needs isolation, this is the
// seam to key by DatabaseInstance rather than being process-global; not
// needed now and would be speculative complexity today.
// ---------------------------------------------------------------------------

namespace {
struct GlobalStatsBox {
	std::mutex mu;
	DriveCallStats stats;
};

GlobalStatsBox &Box() {
	static GlobalStatsBox box;
	return box;
}

void RecordGlobalCall(int64_t DriveCallStats::*counter) {
	auto &box = Box();
	std::lock_guard<std::mutex> guard(box.mu);
	box.stats.*counter += 1;
}

void RecordGlobalRetry() {
	auto &box = Box();
	std::lock_guard<std::mutex> guard(box.mu);
	box.stats.retries += 1;
}
} // namespace

DriveCallStats GetGlobalDriveCallStats() {
	auto &box = Box();
	std::lock_guard<std::mutex> guard(box.mu);
	return box.stats;
}

void ResetGlobalDriveCallStats() {
	auto &box = Box();
	std::lock_guard<std::mutex> guard(box.mu);
	box.stats = DriveCallStats();
}

// A GAUGE, not a counter, and therefore deliberately NOT part of
// DriveCallStats: ResetGlobalDriveCallStats() must not zero it, because the
// cache would still be holding those entries and gdrive_stats() would then
// report a size that is simply false.
std::atomic<uint64_t> &PathCacheGauge() {
	static std::atomic<uint64_t> gauge {0};
	return gauge;
}

void SetGlobalPathCacheEntries(uint64_t entries) {
	PathCacheGauge().store(entries, std::memory_order_relaxed);
}

uint64_t GetGlobalPathCacheEntries() {
	return PathCacheGauge().load(std::memory_order_relaxed);
}

void IncrementGlobalCacheHit() {
	auto &box = Box();
	std::lock_guard<std::mutex> guard(box.mu);
	box.stats.cache_hits += 1;
}

void IncrementGlobalCacheMiss() {
	auto &box = Box();
	std::lock_guard<std::mutex> guard(box.mu);
	box.stats.cache_misses += 1;
}

// ---------------------------------------------------------------------------
// GDriveClientImpl -- the concrete transport.
// ---------------------------------------------------------------------------

namespace {

constexpr const char *kDriveHost = "www.googleapis.com";
constexpr int kDrivePort = 443;

// Retry policy (requirement 6): bounded, jittered exponential backoff.
//   - Up to kMaxAttempts total HTTP attempts per logical call (1 initial +
//     up to kMaxAttempts-1 retries).
//   - Base delay kInitialBackoffMs, doubling each attempt, capped at
//     kMaxBackoffMs.
//   - +/-25% jitter, so concurrent callers hitting the same transient
//     failure don't retry in lockstep (thundering herd).
//   - When Drive supplies a Retry-After, the computed delay is never allowed
//     to be shorter than it.
//   - Only TRANSIENT (5xx) and RATE_LIMIT (429) are retried. Any other 4xx is
//     the caller's problem (bad query, bad field mask, auth, permissions,
//     not-found) and retrying it would just waste 4 more round trips arriving
//     at the same answer.
constexpr int kMaxAttempts = 5;
constexpr int kInitialBackoffMs = 500;
constexpr int kMaxBackoffMs = 20000;

void SleepBackoff(int attempt_number_one_based, int retry_after_seconds) {
	// attempt_number_one_based is the attempt that just failed (1, 2, 3, ...);
	// the delay before the NEXT attempt doubles each time starting at
	// kInitialBackoffMs.
	long long base = static_cast<long long>(kInitialBackoffMs) << (attempt_number_one_based - 1);
	if (base > kMaxBackoffMs || base <= 0) {
		base = kMaxBackoffMs;
	}
	static thread_local std::mt19937 rng(std::random_device {}());
	std::uniform_real_distribution<double> jitter(0.75, 1.25);
	long long delay_ms = static_cast<long long>(static_cast<double>(base) * jitter(rng));
	long long retry_after_ms = static_cast<long long>(retry_after_seconds) * 1000;
	if (retry_after_ms > delay_ms) {
		delay_ms = retry_after_ms;
	}
	if (delay_ms > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
	}
}

std::string BuildMultipartBoundary() {
	static thread_local std::mt19937 rng(std::random_device {}());
	std::uniform_int_distribution<int> dist(0, 15);
	static const char *hex = "0123456789abcdef";
	std::string out = "gdrive_ext_boundary_";
	for (int i = 0; i < 24; ++i) {
		out.push_back(hex[dist(rng)]);
	}
	return out;
}

//! Google's "multipart" upload type: one request containing a JSON metadata
//! part and a media part. This is NOT the full chunked resumable-upload
//! session protocol (initiate -> series of PUTs with Content-Range) that the
//! frozen header's doc comment on Upload() calls "resumable" -- it is a
//! deliberate simplification for a first working transport. It works
//! correctly for whole-file uploads of the sizes this extension's write path
//! produces (HLD section 7: buffered-then-uploaded-once), but does not get
//! resumability across a dropped connection on a very large upload. Flagged
//! in the report as follow-up work once live credentials exist to test the
//! larger-file path.
std::string BuildMultipartBody(const std::string &boundary, const std::string &metadata_json,
                                const std::string &content_type, const std::string &data) {
	std::ostringstream body;
	body << "--" << boundary << "\r\n";
	body << "Content-Type: application/json; charset=UTF-8\r\n\r\n";
	body << metadata_json << "\r\n";
	body << "--" << boundary << "\r\n";
	body << "Content-Type: " << (content_type.empty() ? "application/octet-stream" : content_type) << "\r\n\r\n";
	body << data << "\r\n";
	body << "--" << boundary << "--\r\n";
	return body.str();
}

//! Minimal JSON string-escaping sufficient for the handful of values (file
//! names, ids) that end up embedded in a metadata part or a PATCH body here.
//! Not a general-purpose JSON writer -- picojson only gives us a parser API
//! we use for that, so hand-rolling escaping for the few string values we
//! ever emit is simpler than pulling in a JSON serializer for four call sites.
std::string JsonEscape(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back(c);
			}
		}
	}
	return out;
}

} // namespace

class GDriveClientImpl : public GDriveClient {
public:
	GDriveClientImpl(std::string access_token, std::string drive_id)
	    : access_token_(std::move(access_token)), drive_id_(std::move(drive_id)) {
	}

	DriveResponse GetMetadata(const std::string &file_id) override {
		std::string path = "/drive/v3/files/" + UrlEncode(file_id) + "?fields=" + UrlEncode(FileFieldsMask()) + "&" +
		                    AllDrivesParams();
		return ExecuteWithRetry(HttpMethod::GET, path, {}, "", "", &DriveCallStats::files_get, {200});
	}

	DriveResponse List(const std::string &query, const std::string &page_token) override {
		std::vector<DriveFileMeta> all_files;
		std::string token = page_token;
		DriveResponse last;

		// Follows nextPageToken to exhaustion (requirement 2) -- a caller must
		// never observe a partial listing. Each page is a separate counted
		// files_list call; if any page fails, the failure (with whatever was
		// already accumulated discarded) is returned rather than a silently
		// truncated success.
		for (;;) {
			std::ostringstream qs;
			qs << "/drive/v3/files?q=" << UrlEncode(query) << "&pageSize=1000&fields=" << UrlEncode(ListFieldsMask())
			   << "&" << AllDrivesParams();
			if (!drive_id_.empty()) {
				qs << "&corpora=drive&driveId=" << UrlEncode(drive_id_);
			}
			if (!token.empty()) {
				qs << "&pageToken=" << UrlEncode(token);
			}

			last = ExecuteWithRetry(HttpMethod::GET, qs.str(), {}, "", "", &DriveCallStats::files_list, {200});
			if (!last.ok) {
				return last;
			}

			std::vector<DriveFileMeta> page_files;
			std::string next_token;
			if (!ParseFileList(last.body, page_files, next_token)) {
				DriveResponse bad;
				bad.ok = false;
				bad.http_status = last.http_status;
				bad.error.kind = GDriveErrorKind::UNKNOWN;
				bad.error.message = "malformed files.list response body";
				return bad;
			}
			for (auto &f : page_files) {
				all_files.push_back(std::move(f));
			}
			if (next_token.empty()) {
				break;
			}
			token = next_token;
		}

		// Re-serialize the fully-paginated result as one body so the caller's
		// ParseFileList() sees the complete set with an empty next_page_token
		// (there is nothing left to follow).
		picojson::array files_array;
		files_array.reserve(all_files.size());
		for (const auto &f : all_files) {
			picojson::object obj;
			obj["id"] = picojson::value(f.id);
			obj["name"] = picojson::value(f.name);
			obj["mimeType"] = picojson::value(f.mime_type);
			if (f.size >= 0) {
				obj["size"] = picojson::value(std::to_string(f.size));
			}
			obj["modifiedTime"] = picojson::value(f.modified_time);
			obj["headRevisionId"] = picojson::value(f.head_revision_id);
			files_array.emplace_back(obj);
		}
		picojson::object top;
		top["files"] = picojson::value(files_array);

		DriveResponse out;
		out.ok = true;
		out.http_status = 200;
		out.body = picojson::value(top).serialize();
		return out;
	}

	DriveResponse Download(const std::string &file_id, int64_t start, int64_t end) override {
		const bool trace = getenv("GDRIVE_TRACE_RANGES") != nullptr;
		auto t0 = std::chrono::steady_clock::now();
		auto out = ExecuteWithRetry(HttpMethod::GET,
		                            "/drive/v3/files/" + UrlEncode(file_id) + "?alt=media&" + AllDrivesParams(),
		                            {{"Range", BuildRangeHeader(start, end)}}, "", "",
		                            &DriveCallStats::files_media, {200, 206});
		if (trace) {
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
			              .count();
			fprintf(stderr, "GDRIVE_RANGE off=%lld len=%lld ms=%lld\n", (long long)start,
			        (long long)(end - start + 1), (long long)ms);
		}
		return out;
	}

	DriveResponse DownloadUnused(const std::string &file_id, int64_t start, int64_t end) {
		std::string path =
		    "/drive/v3/files/" + UrlEncode(file_id) + "?alt=media&" + AllDrivesParams();
		std::vector<std::pair<std::string, std::string>> headers = {{"Range", BuildRangeHeader(start, end)}};
		// Both 200 (server chose to ignore Range, e.g. whole small file) and
		// 206 (Partial Content, the normal case) are success -- requirement 4.
		return ExecuteWithRetry(HttpMethod::GET, path, headers, "", "", &DriveCallStats::files_media, {200, 206});
	}

	DriveResponse Export(const std::string &file_id, const std::string &mime_type) override {
		// files.export does not support Range (HLD section 6) -- no Range
		// header is ever sent here, by design, not by omission.
		std::string path = "/drive/v3/files/" + UrlEncode(file_id) + "/export?mimeType=" + UrlEncode(mime_type) +
		                    "&" + AllDrivesParams();
		return ExecuteWithRetry(HttpMethod::GET, path, {}, "", "", &DriveCallStats::files_export, {200});
	}

	DriveResponse GenerateFileId() override {
		std::string path = "/drive/v3/files/generateIds?count=1&space=drive&" + AllDrivesParams();
		return ExecuteWithRetry(HttpMethod::GET, path, {}, "", "", &DriveCallStats::files_get, {200});
	}

	DriveResponse Upload(const std::string &file_id, const std::string &parent_id, const std::string &name,
	                     const std::string &content_type, const std::string &data,
	                     const std::string &reserved_id) override {
		// BUG FIX (live run 2026-07-26): `content_type` is a real MIME type
		// for every caller EXCEPT MutateCreateDirectory, which passes Drive's
		// pseudo-type "application/vnd.google-apps.folder" -- not a valid
		// media type for uploaded bytes, because a folder has no bytes at
		// all. Passing it as the *media part's* Content-Type (the old
		// behaviour) makes Google reject the whole request with "Invalid
		// MIME type provided for the uploaded content." even though the
		// metadata part is perfectly well-formed. Folder creation needs no
		// media part whatsoever -- Drive learns the type from metadata's
		// "mimeType" field -- so it is sent as a plain JSON files.create,
		// never as a multipart upload.
		const bool is_folder_create = content_type == kFolderMimeType;

		std::ostringstream metadata;
		metadata << "{";
		bool first = true;
		if (!name.empty()) {
			metadata << "\"name\":\"" << JsonEscape(name) << "\"";
			first = false;
		}
		if (!parent_id.empty() && file_id.empty()) {
			// Parents are only set on create; files.update (an existing file
			// being overwritten) leaves parents untouched here -- MoveFile is
			// the dedicated path for changing parents (requirement: don't
			// conflate "write new content" with "move").
			if (!first) {
				metadata << ",";
			}
			metadata << "\"parents\":[\"" << JsonEscape(parent_id) << "\"]";
			first = false;
		}
		if (is_folder_create) {
			if (!first) {
				metadata << ",";
			}
			metadata << "\"mimeType\":\"" << JsonEscape(content_type) << "\"";
			first = false;
		}
		if (!reserved_id.empty() && file_id.empty()) {
			// A CREATE with a pre-reserved id. Drive rejects a second create
			// with the same id (409), which is exactly what makes the request
			// safe to retry -- see GenerateFileId.
			if (!first) {
				metadata << ",";
			}
			metadata << "\"id\":\"" << JsonEscape(reserved_id) << "\"";
		}
		metadata << "}";

		if (is_folder_create) {
			// Plain files.create, no /upload/ prefix and no media part --
			// there is nothing to upload.
			std::string path = "/drive/v3/files?fields=" + UrlEncode(FileFieldsMask()) + "&" + AllDrivesParams();
			return ExecuteWithRetry(HttpMethod::POST, path, {}, metadata.str(), "application/json; charset=UTF-8",
			                        &DriveCallStats::files_create, {200});
		}

		std::string boundary = BuildMultipartBoundary();
		std::string body = BuildMultipartBody(boundary, metadata.str(), content_type, data);
		std::string content_type_header = "multipart/related; boundary=" + boundary;

		std::ostringstream path;
		if (file_id.empty()) {
			path << "/upload/drive/v3/files?uploadType=multipart&fields=" << UrlEncode(FileFieldsMask()) << "&"
			     << AllDrivesParams();
			if (reserved_id.empty()) {
				return ExecuteWithRetry(HttpMethod::POST, path.str(), {}, body, content_type_header,
				                        &DriveCallStats::files_create, {200});
			}
			// With a reserved id the create IS retryable: a duplicate attempt
			// cannot make a second file, it can only collide with the first.
			auto resp = ExecuteWithRetry(HttpMethod::POST, path.str(), {}, body, content_type_header,
			                             &DriveCallStats::files_create, {200}, /*retry_non_idempotent=*/true);
			if (!resp.ok && resp.http_status == 409) {
				// 409 "A file already exists with the provided ID" means an
				// earlier attempt DID land and we only lost its response.
				// That is a success, not a failure -- report the file.
				return GetMetadata(reserved_id);
			}
			return resp;
		}
		path << "/upload/drive/v3/files/" << UrlEncode(file_id) << "?uploadType=multipart&fields="
		     << UrlEncode(FileFieldsMask()) << "&" << AllDrivesParams();
		return ExecuteWithRetry(HttpMethod::PATCH, path.str(), {}, body, content_type_header,
		                        &DriveCallStats::files_update, {200});
	}

	DriveResponse Delete(const std::string &file_id, bool permanent) override {
		std::string path = "/drive/v3/files/" + UrlEncode(file_id) + "?" + AllDrivesParams();
		if (permanent) {
			// files.delete: 204 No Content on success, empty body.
			return ExecuteWithRetry(HttpMethod::DELETE_, path, {}, "", "", &DriveCallStats::files_delete, {204});
		}
		// D-6: default is trash, which is a files.update({trashed: true}) under
		// the hood. Still counted as files_delete -- callers of Delete() are
		// asking for a delete-shaped operation regardless of the HTTP verb
		// Drive happens to use for the trash variant.
		std::string body = "{\"trashed\":true}";
		return ExecuteWithRetry(HttpMethod::PATCH, path + "&fields=" + UrlEncode(FileFieldsMask()), {}, body,
		                        "application/json", &DriveCallStats::files_delete, {200});
	}

	DriveResponse Move(const std::string &file_id, const std::string &new_parent_id, const std::string &new_name,
	                   const std::string &old_parent_id) override {
		std::ostringstream path;
		path << "/drive/v3/files/" << UrlEncode(file_id) << "?fields=" << UrlEncode(FileFieldsMask()) << "&"
		     << AllDrivesParams();
		if (!new_parent_id.empty()) {
			path << "&addParents=" << UrlEncode(new_parent_id);
		}
		if (!old_parent_id.empty()) {
			path << "&removeParents=" << UrlEncode(old_parent_id);
		}
		std::string body = "{}";
		if (!new_name.empty()) {
			body = "{\"name\":\"" + JsonEscape(new_name) + "\"}";
		}
		return ExecuteWithRetry(HttpMethod::PATCH, path.str(), {}, body, "application/json",
		                        &DriveCallStats::files_update, {200});
	}

	const DriveCallStats &Stats() const override {
		return stats_;
	}

	void ResetStats() override {
		// Resets THIS instance's view only. The process-wide aggregate
		// (gdrive_stats.hpp) is deliberately independent -- see the comment on
		// RecordGlobalCall below for why.
		stats_ = DriveCallStats();
	}

private:
	enum class HttpMethod { GET, POST, PATCH, DELETE_ };

	//! Is retrying this method after an AMBIGUOUS failure safe?
	//!
	//! RFC 9110 idempotency: GET and DELETE may be repeated with the same
	//! end state. POST may not -- a repeated files.create makes a second
	//! file. PATCH is idempotent in principle, but Drive's files.update is
	//! used here both to move a file and to trash it, and a retry that races
	//! another writer can reapply a stale intent, so it is excluded too.
	//! Conservative on purpose: the cost of being wrong is a duplicate file
	//! that R-4 then reports as an ambiguous path forever.
	static bool IsIdempotent(HttpMethod method) {
		return method == HttpMethod::GET || method == HttpMethod::DELETE_;
	}

	struct RawResult {
		bool transport_ok = false;
		int status = 0;
		std::string body;
		std::string retry_after;
	};

	RawResult DoHttp(HttpMethod method, const std::string &path_and_query,
	                 const std::vector<std::pair<std::string, std::string>> &extra_headers, const std::string &body,
	                 const std::string &content_type) {
		// ---------------------------------------------------------------
		// One TLS connection PER THREAD, reused for every request that
		// thread makes.
		//
		// This used to construct an SSLClient here, per call -- a fresh TCP
		// connection and a full TLS handshake for every single request.
		// Measured on the benchmark query: 35 requests, median 1558 ms each,
		// minimum 1298 ms even for a 128 KB read. Against a marginal
		// transfer rate of roughly 160 MB/s, essentially all of that was
		// connection setup.
		//
		// thread_local rather than a shared pool because httplib's Client
		// owns a socket and is not safe for concurrent use. DuckDB's scan
		// gives each thread its own file handle, so per-thread is both the
		// natural granularity and lock-free. Connections survive across
		// queries, so a second query on a warm thread pays nothing.
		//
		// The bearer token is a PER-REQUEST header, never a property of the
		// connection, so reusing one across secrets leaks nothing -- the
		// same reason ordinary HTTP connection pools are safe.
		// ---------------------------------------------------------------
		static thread_local duckdb_httplib_openssl::SSLClient client(kDriveHost, kDrivePort);
		static thread_local bool configured = false;
		if (!configured) {
			client.enable_server_certificate_verification(true);
			client.set_connection_timeout(std::chrono::seconds(10));
			client.set_read_timeout(std::chrono::seconds(60));
			client.set_write_timeout(std::chrono::seconds(60));
			client.set_keep_alive(true);
			configured = true;
		}

		duckdb_httplib_openssl::Headers headers;
		// REQ-NF-03: the bearer token lives ONLY in this header, built fresh
		// per attempt, never copied into a string that could end up in a log
		// or an error. Nothing in this file logs request headers.
		headers.emplace("Authorization", "Bearer " + access_token_);
		for (const auto &kv : extra_headers) {
			headers.emplace(kv.first, kv.second);
		}

		duckdb_httplib_openssl::Result res;
		switch (method) {
		case HttpMethod::GET:
			res = client.Get(path_and_query.c_str(), headers);
			break;
		case HttpMethod::POST:
			res = client.Post(path_and_query.c_str(), headers, body, content_type);
			break;
		case HttpMethod::PATCH:
			res = client.Patch(path_and_query.c_str(), headers, body, content_type);
			break;
		case HttpMethod::DELETE_:
			res = client.Delete(path_and_query.c_str(), headers);
			break;
		}

		RawResult out;
		if (!res) {
			out.transport_ok = false;
			return out;
		}
		out.transport_ok = true;
		out.status = res->status;
		out.body = std::move(res->body);
		auto it = res->headers.find("Retry-After");
		if (it != res->headers.end()) {
			out.retry_after = it->second;
		}
		return out;
	}

	DriveResponse ExecuteWithRetry(HttpMethod method, const std::string &path_and_query,
	                               const std::vector<std::pair<std::string, std::string>> &extra_headers,
	                               const std::string &body, const std::string &content_type,
	                               int64_t DriveCallStats::*counter, const std::vector<int> &success_statuses,
	                               bool retry_non_idempotent = false) {
		int attempt = 0;
		for (;;) {
			++attempt;
			stats_.*counter += 1;
			RecordGlobalCall(counter);

			RawResult raw = DoHttp(method, path_and_query, extra_headers, body, content_type);

			if (!raw.transport_ok) {
				// A transport-level failure (DNS, connect, TLS, timeout) never
				// reaches ClassifyDriveError (there is no HTTP status or body
				// to classify) -- treat it as TRANSIENT so it gets the same
				// bounded retry treatment as a 5xx.
				//
				// ...but ONLY for idempotent methods. A transport failure is
				// ambiguous by definition: the request may have been fully
				// processed and only the RESPONSE lost. Retrying a
				// `POST files.create` in that state produces a SECOND file
				// with the same name in the same parent.
				//
				// That is worse here than in most filesystems, because
				// duplicate names in one folder are a hard error by design
				// (R-4). A retry can therefore poison a path permanently, and
				// the user sees "ambiguous" on a path they wrote exactly once
				// -- with no way to tell which copy is theirs.
				//
				// A clear failure the caller can retry deliberately beats a
				// silent duplicate. The real fix is Drive's resumable upload
				// protocol, whose session URI makes a retry idempotent; that
				// is tracked in docs/reviews/2026-07-26-codex-review-2-read-
				// path.md, finding 1, and is a piece of work rather than a
				// patch.
				if ((retry_non_idempotent || IsIdempotent(method)) && attempt < kMaxAttempts) {
					stats_.retries += 1;
					RecordGlobalRetry();
					SleepBackoff(attempt, 0);
					continue;
				}
				DriveResponse response;
				response.ok = false;
				response.http_status = 0;
				response.error.kind = GDriveErrorKind::TRANSIENT;
				if (retry_non_idempotent || IsIdempotent(method)) {
					response.error.message = "network transport failure (no response from Drive)";
				} else {
					// Say plainly that the outcome is unknown. "Failed" would
					// imply nothing happened, and the user could safely retry
					// -- but Drive may have committed the change and only the
					// response was lost, in which case a manual retry creates
					// the duplicate we just declined to create automatically.
					response.error.message =
					    "network transport failure with no response from Drive; the change may or may "
					    "not have been applied. It was NOT retried automatically, because repeating a "
					    "write whose outcome is unknown can create a second file with the same name "
					    "(a path with duplicate names cannot be addressed). Check the destination "
					    "before retrying.";
				}
				return response;
			}

			bool success = std::find(success_statuses.begin(), success_statuses.end(), raw.status) !=
			               success_statuses.end();
			if (success) {
				DriveResponse response;
				response.ok = true;
				response.http_status = raw.status;
				response.body = std::move(raw.body);
				return response;
			}

			GDriveError err = ClassifyDriveError(raw.status, raw.body, raw.retry_after);
			bool retryable = (err.kind == GDriveErrorKind::TRANSIENT || err.kind == GDriveErrorKind::RATE_LIMIT);
			if (retryable && attempt < kMaxAttempts) {
				stats_.retries += 1;
				RecordGlobalRetry();
				SleepBackoff(attempt, err.retry_after_seconds);
				continue;
			}

			DriveResponse response;
			response.ok = false;
			response.http_status = raw.status;
			response.body = raw.body;
			response.error = err;
			return response;
		}
	}

	std::string access_token_;
	std::string drive_id_;
	DriveCallStats stats_;
};

} // namespace gdrive
} // namespace duckdb

// ---------------------------------------------------------------------------
// Factory + global stats accessors declared in gdrive_stats.hpp.
// ---------------------------------------------------------------------------
namespace duckdb {
namespace gdrive {

std::unique_ptr<GDriveClient> CreateGDriveClient(const std::string &access_token, const std::string &drive_id) {
	return std::make_unique<GDriveClientImpl>(access_token, drive_id);
}

} // namespace gdrive
} // namespace duckdb

#endif // GDRIVE_CLIENT_PURE_ONLY
