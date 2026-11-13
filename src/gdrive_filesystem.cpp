// S-2.2/S-2.6..S-2.19, S-3.10..S-3.13 -- GDriveFileSystem: registration,
// resolution, reads, listing/glob, and the small always-throw surface
// (positional Write, Truncate, Trim). See gdrive_filesystem.hpp for the
// contract and docs/hld.md sections 3, 4, 6, 8 for the design this follows.
#include "gdrive_internal.hpp"
#include "gdrive_glob.hpp"
#include "gdrive_weburl.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include "gdrive_banner.hpp"
#include "duckdb_compat.hpp"

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Small internal helpers (gdrive_internal.hpp).
// ---------------------------------------------------------------------------

std::string EscapeDriveQueryLiteral(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for (char c : value) {
		if (c == '\\' || c == '\'') {
			out.push_back('\\');
		}
		out.push_back(c);
	}
	return out;
}

[[noreturn]] void ThrowGDriveError(const GDriveError &error, const std::string &context) {
	auto message = FormatUserMessage(error, context);
	switch (ExceptionTypeFor(error.kind)) {
	case GDriveExceptionType::PERMISSION:
		throw PermissionException(message);
	case GDriveExceptionType::INVALID_INPUT:
		throw InvalidInputException(message);
	case GDriveExceptionType::IO:
	default:
		throw IOException(message);
	}
}

void RefuseReplacing(const DriveFileMeta &existing, const std::string &target) {
	if (existing.locked) {
		// Drive refuses a content write to a locked file, but not the trash
		// and move that COPY's tmp-file staging ends in: without this the
		// lock would be silently bypassed (found live 2026-11-13).
		throw PermissionException(
		    "gdrive: cannot write to '%s': it is locked (a Drive content restriction makes it read-only). "
		    "Unlock it in Drive first, or write under another name.",
		    target);
	}
	if (existing.IsFolder()) {
		throw IOException("gdrive: cannot write to '%s': it is a folder, not a file", target);
	}
	if (existing.IsNativeGoogleFormat()) {
		throw InvalidInputException(
		    "gdrive: cannot write to '%s': it is a native Google file (%s), which has no bytes to replace -- writing "
		    "would destroy its content. Write a file under another name instead.",
		    target, existing.mime_type);
	}
}

std::string JoinGDrivePath(const std::string &parent_path, const std::string &name) {
	std::string out = GDRIVE_SCHEME;
	if (!parent_path.empty()) {
		out += parent_path;
		out += "/";
	}
	out += name;
	return out;
}

bool GetBoolSetting(optional_ptr<FileOpener> opener, const std::string &name, bool default_value) {
	if (!opener) {
		return default_value;
	}
	Value value;
	if (duckdb_compat::TryGetSetting(opener, name, value)) {
		return value.GetValue<bool>();
	}
	return default_value;
}

uint64_t GetUBigIntSetting(optional_ptr<FileOpener> opener, const std::string &name, uint64_t default_value) {
	if (!opener) {
		return default_value;
	}
	Value value;
	if (duckdb_compat::TryGetSetting(opener, name, value)) {
		return value.GetValue<uint64_t>();
	}
	return default_value;
}

std::string GetStringSetting(optional_ptr<FileOpener> opener, const std::string &name,
                              const std::string &default_value) {
	if (!opener) {
		return default_value;
	}
	Value value;
	if (duckdb_compat::TryGetSetting(opener, name, value)) {
		return value.ToString();
	}
	return default_value;
}

// Secrets and settings resolve through the opener, which works for a client
// connection AND for the database-level opener DuckDB uses when it opens an
// ATTACHed database file (which has no ClientContext).
optional_ptr<FileOpener> RequireOpener(optional_ptr<FileOpener> opener, const std::string &context) {
	if (!FileOpener::TryGetSecretManager(opener)) {
		throw IOException("gdrive: no database context available to resolve '%s'", context);
	}
	return opener;
}

namespace {

//! RFC 3339 (Drive's modifiedTime format) -> DuckDB timestamp. Drive always
//! sends a 'T' separator and either a 'Z' or a numeric offset; Timestamp::
//! FromString expects a space separator, so translate before parsing.
timestamp_t ParseRfc3339(const std::string &rfc3339) {
	if (rfc3339.empty()) {
		return timestamp_t(0);
	}
	std::string normalised = rfc3339;
	auto t_pos = normalised.find('T');
	if (t_pos != std::string::npos) {
		normalised[t_pos] = ' ';
	}
	if (!normalised.empty() && normalised.back() == 'Z') {
		normalised.pop_back();
		normalised += "+00";
	}
	try {
		return Timestamp::FromString(normalised, true);
	} catch (...) {
		return timestamp_t(0);
	}
}

std::string RootFolderFor(const GDriveAuthContext &auth) {
	if (!auth.root_folder_id.empty()) {
		return auth.root_folder_id;
	}
	if (!auth.drive_id.empty()) {
		return auth.drive_id;
	}
	return "root";
}

//! One files.list call for children of `parent_id` named exactly `name`,
//! following pagination to exhaustion (a caller must never see a partial
//! listing -- that is how "the glob silently dropped files" bugs happen).
std::vector<DriveFileMeta> ListByName(GDriveClient &client, const std::string &parent_id, const std::string &name,
                                     bool require_folder, const std::string &context_path) {
	std::string query =
	    "'" + parent_id + "' in parents and name='" + EscapeDriveQueryLiteral(name) + "' and trashed=false";
	if (require_folder) {
		query += " and mimeType='application/vnd.google-apps.folder'";
	}

	std::vector<DriveFileMeta> matches;
	std::string page_token;
	do {
		auto resp = client.List(query, page_token);
		if (!resp.ok) {
			ThrowGDriveError(resp.error, context_path);
		}
		std::vector<DriveFileMeta> page;
		std::string next;
		if (!ParseFileList(resp.body, page, next)) {
			throw IOException("gdrive: malformed listing response resolving '%s'", context_path);
		}
		matches.insert(matches.end(), page.begin(), page.end());
		page_token = next;
	} while (!page_token.empty());
	// Drive compares names in `q` case-INSENSITIVELY ("name='report.csv'"
	// also matches "Report.csv"), but stores them case-sensitively and lets
	// both live in one folder. Without this filter, writing report.csv
	// replaced a colleague's Report.csv (found by exploratory testing,
	// 2026-11-07). Keep only the exact name.
	matches.erase(std::remove_if(matches.begin(), matches.end(),
	                             [&](const DriveFileMeta &m) { return m.name != name; }),
	              matches.end());
	return matches;
}

//! One files.list call for ALL children of `parent_id`, paginated to
//! exhaustion. Used by ListFiles/Glob, which need the whole folder rather
//! than one name.
//!
//! `file_fields` non-empty asks the listing for those fields as well, and
//! `rows` (then required) receives the FileRow of each child, in the same
//! order as the returned metadata.
std::vector<DriveFileMeta> ListChildren(GDriveClient &client, const std::string &parent_id,
                                        const std::string &context_path, const std::string &file_fields = "",
                                        std::vector<FileRow> *rows = nullptr) {
	std::string query = "'" + parent_id + "' in parents and trashed=false";
	std::vector<DriveFileMeta> children;
	std::string page_token;
	do {
		auto resp = client.List(query, page_token, file_fields);
		if (!resp.ok) {
			ThrowGDriveError(resp.error, context_path);
		}
		std::vector<DriveFileMeta> page;
		std::string next;
		if (!ParseFileList(resp.body, page, next)) {
			throw IOException("gdrive: malformed listing response for '%s'", context_path);
		}
		if (rows) {
			// Same body, same order: ParseFileList skips only objects without
			// an id, and ParseFileRowList rejects those outright.
			std::vector<FileRow> page_rows;
			std::string row_next, error;
			if (!ParseFileRowList(resp.body, page_rows, row_next, error) || page_rows.size() != page.size()) {
				throw IOException("gdrive: malformed listing response for '%s': %s", context_path, error);
			}
			rows->insert(rows->end(), page_rows.begin(), page_rows.end());
		}
		children.insert(children.end(), page.begin(), page.end());
		page_token = next;
	} while (!page_token.empty());
	return children;
}

//! R-4 / S-2.8: more than one match for one name in one folder is an error
//! naming both (or more) ids and pointing at the id: form. Never silently
//! pick one -- that would make results depend on Drive's internal ordering.
[[noreturn]] void ThrowAmbiguous(const std::vector<DriveFileMeta> &matches, const std::string &context_path) {
	std::string ids;
	for (size_t i = 0; i < matches.size(); i++) {
		if (i > 0) {
			ids += ", ";
		}
		ids += matches[i].id;
	}
	throw IOException("gdrive: '%s' is ambiguous: %d files share that name in the same folder (ids: %s). "
	                  "Use the gdrive://id:<fileId> form to address one of them directly.",
	                  context_path, static_cast<int>(matches.size()), ids);
}

} // namespace

namespace {
//! Defined below, next to the code they belong with; declared here because
//! OpenFile and Read are above them and this file is ordered by narrative
//! rather than by dependency.
void ReadExactRangeTail(const std::string &path, void *buffer, int64_t nr_bytes, idx_t location,
                        const DriveResponse &resp);
} // namespace

std::string BuildBlockKey(const GDriveAuthContext &auth, const DriveFileMeta &meta);

namespace {
//! COPY ... TO 'gdrive://id:<id>' stages its output at "tmp_" + the file
//! name: 'gdrive:/tmp_id:<id>' on DuckDB 1.5 (JoinPath), 'gdrive://tmp_id:<id>'
//! on 2.0 (ReplaceFileName). Neither names a real file -- the id names one --
//! so both are the target itself: written in place, moved onto itself (a
//! no-op), and never removed by a failed COPY's cleanup.
//! And on 1.5 the move back to the target computes 'gdrive:/id:<id>'.
const char *const kStagingAliases[] = {"gdrive:/tmp_id:", "gdrive://tmp_id:", "gdrive:/id:"};

bool IsCopyStagingAlias(const std::string &path) {
	for (auto alias : kStagingAliases) {
		if (path.rfind(alias, 0) == 0) {
			return true;
		}
	}
	return false;
}
} // namespace

std::string ToGDrivePath(const std::string &path) {
	for (auto alias : kStagingAliases) {
		if (path.rfind(alias, 0) == 0) {
			return std::string(GDRIVE_SCHEME) + "id:" + path.substr(std::strlen(alias));
		}
	}
	std::string out, error;
	if (!WebUrlToGDrivePath(path, out, error)) {
		throw InvalidInputException("gdrive: %s", error);
	}
	return out;
}

namespace {
//! FileExists / DirectoryExists answer for speculative probes and must not
//! throw: an address on a Drive host that names nothing simply does not exist.
bool TryToGDrivePath(const std::string &path, std::string &out) {
	if (IsCopyStagingAlias(path)) {
		out = ToGDrivePath(path);
		return true;
	}
	std::string error;
	return WebUrlToGDrivePath(path, out, error);
}
//! An earlier version (gdrive://...?version=<id>) can be read, nothing else:
//! writing, moving or deleting "a version" would act on the CURRENT file.
void RefuseVersion(const GDriveUri &uri, const char *operation) {
	if (!uri.version.empty()) {
		throw InvalidInputException("gdrive: cannot %s '%s': an earlier version of a file is read-only", operation,
		                            uri.ToString());
	}
}

//! A ranged read of whatever the handle reads: the file, or the revision it
//! names.
DriveResponse DownloadFor(GDriveClient &client, const GDriveFileHandleImpl &h, int64_t start, int64_t end) {
	return h.version.empty() ? client.Download(h.meta.id, start, end)
	                         : client.DownloadRevision(h.meta.id, h.version, start, end);
}
} // namespace

std::string CanonicalPathOf(const GDriveUri &uri) {
	if (uri.kind != GDriveUriKind::PATH) {
		return "";
	}
	std::string canonical;
	for (auto &segment : uri.segments) {
		canonical += (canonical.empty() ? "" : "/") + segment;
	}
	return canonical;
}

//! Engine behind ResolvePath. Returns false ONLY for the "no such file or
//! directory" outcome -- everything else (ambiguity, auth, malformed
//! response, transient API errors) throws. This distinction matters: Glob's
//! not-found-is-empty-not-error semantics must swallow exactly the former
//! and NEVER the latter -- silently turning an R-4 ambiguity or an auth
//! failure into "zero matches" would be the same class of bug REQ-F-08 and
//! R-2 exist to prevent, just one layer up.
bool TryResolvePath(GDrivePathCache &cache, GDriveClient &client, const GDriveAuthContext &auth, const GDriveUri &uri,
                    DriveFileMeta &out, bool *leaf_is_fresh) {
	std::string context_path = uri.ToString();

	// Default to "not fresh", the conservative answer: a caller skips its
	// refresh only on positive evidence. Every early return leaves it false.
	if (leaf_is_fresh) {
		*leaf_is_fresh = false;
	}

	if (uri.kind == GDriveUriKind::FILE_ID) {
		// Direct form: zero resolution (files.list) calls (S-2.9). The single
		// files.get below is metadata lookup, not resolution.
		//
		// Served from the metadata cache when it is warm. DuckDB opens a
		// handle PER THREAD for a parallel scan, so without this the id:
		// form -- the one documented as the zero-round-trip fast path --
		// still cost one files.get per thread.
		CacheKey meta_key;
		meta_key.identity = auth.identity;
		meta_key.drive_id = auth.drive_id;
		meta_key.root_folder_id = auth.root_folder_id;
		return cache.GetOrFetchMetadata(meta_key, uri.file_id,
		                                 [&](DriveFileMeta &fresh) {
			                                 auto resp = client.GetMetadata(uri.file_id);
			                                 if (!resp.ok) {
				                                 if (resp.error.kind == GDriveErrorKind::NOT_FOUND) {
					                                 return false;
				                                 }
				                                 ThrowGDriveError(resp.error, context_path);
			                                 }
			                                 if (!ParseFileMeta(resp.body, fresh)) {
				                                 throw IOException(
				                                     "gdrive: malformed metadata response for '%s'", context_path);
			                                 }
			                                 // A path never resolves to a trashed file (listings say
			                                 // trashed=false); an id must not either. It used to: an
			                                 // id: kept for a file COPY since replaced read the old,
			                                 // trashed content without a word.
			                                 return !fresh.trashed;
		                                 },
		                                 out);
	}

	std::string parent_id = RootFolderFor(auth);
	std::string canonical;
	DriveFileMeta meta;
	bool have_meta = false;

	for (size_t i = 0; i < uri.segments.size(); i++) {
		const auto &segment = uri.segments[i];
		canonical += (canonical.empty() ? "" : "/") + segment;
		bool is_last = (i + 1 == uri.segments.size());

		CacheKey key {auth.identity, auth.drive_id, auth.root_folder_id, canonical};
		DriveFileMeta cached;
		bool cached_fresh = false;
		if (cache.TryGet(key, cached, &cached_fresh)) {
			meta = cached;
			have_meta = true;
			parent_id = meta.id;
			if (is_last && leaf_is_fresh) {
				// A hit on an entry this query's own glob just listed. Common:
				// DuckDB's multi-file reader globs before it opens.
				*leaf_is_fresh = cached_fresh;
			}
			continue;
		}

		auto matches = ListByName(client, parent_id, segment, !is_last, context_path);
		if (matches.empty()) {
			return false;
		}
		if (matches.size() > 1) {
			ThrowAmbiguous(matches, context_path);
		}
		meta = matches[0];
		have_meta = true;
		cache.Put(key, meta);
		parent_id = meta.id;
		if (is_last && leaf_is_fresh) {
			// This leaf came straight off a files.list whose field mask
			// includes size and headRevisionId, so it is as fresh as a
			// files.get would be.
			*leaf_is_fresh = true;
		}
	}

	if (!have_meta) {
		// The root itself was requested (e.g. "gdrive://" alone, or a
		// zero-segment parent path).
		auto resp = client.GetMetadata(parent_id);
		if (!resp.ok) {
			if (resp.error.kind == GDriveErrorKind::NOT_FOUND) {
				return false;
			}
			ThrowGDriveError(resp.error, context_path);
		}
		if (!ParseFileMeta(resp.body, meta)) {
			throw IOException("gdrive: malformed metadata response for '%s'", context_path);
		}
	}
	out = meta;
	return true;
}

DriveFileMeta ResolvePath(GDrivePathCache &cache, GDriveClient &client, const GDriveAuthContext &auth,
                          const GDriveUri &uri, bool *leaf_is_fresh) {
	DriveFileMeta meta;
	if (!TryResolvePath(cache, client, auth, uri, meta, leaf_is_fresh)) {
		throw IOException("gdrive: no such file or directory: '%s'", uri.ToString());
	}
	return meta;
}

// ---------------------------------------------------------------------------
// GDriveFileSystem
// ---------------------------------------------------------------------------

GDriveFileSystem::GDriveFileSystem() {
}

namespace {
//! DatabaseInstance -> its GDriveFileSystem. The VFS owns the filesystem and
//! offers no typed lookup, so the table functions find it here. One entry per
//! database the extension is loaded into; the filesystem removes its own on
//! destruction, which happens with its database.
std::mutex &RegistryLock() {
	static std::mutex lock;
	return lock;
}
std::unordered_map<const DatabaseInstance *, GDriveFileSystem *> &Registry() {
	static std::unordered_map<const DatabaseInstance *, GDriveFileSystem *> registry;
	return registry;
}
} // namespace

GDriveFileSystem::~GDriveFileSystem() {
	if (attached_db) {
		std::lock_guard<std::mutex> guard(RegistryLock());
		auto it = Registry().find(attached_db);
		if (it != Registry().end() && it->second == this) {
			Registry().erase(it);
		}
	}
}

void GDriveFileSystem::AttachToDatabase(DatabaseInstance &db) {
	std::lock_guard<std::mutex> guard(RegistryLock());
	attached_db = &db;
	Registry()[&db] = this;
}

GDriveFileSystem &GDriveFileSystem::ForDatabase(DatabaseInstance &db) {
	std::lock_guard<std::mutex> guard(RegistryLock());
	auto it = Registry().find(&db);
	if (it == Registry().end()) {
		throw InternalException("gdrive: the gdrive filesystem is not registered with this database");
	}
	return *it->second;
}

bool GDriveFileSystem::CanHandleFile(const string &fpath) {
	return IsGDriveUri(fpath) || IsDriveWebUrl(fpath) || IsCopyStagingAlias(fpath);
}

DriveFileMeta GDriveFileSystem::ResolveOrThrow(optional_ptr<FileOpener> context, const GDriveUri &uri) {
	if (!HasAnyGDriveSecret(context)) {
		throw IOException(
		    "gdrive: no gdrive secret configured for '%s' -- %s", uri.ToString(), kNoSecretAdvice);
	}
	auto auth = GetAuthContext(context, uri.ToString());
	auto client = CreateGDriveClient(auth);
	return ResolvePath(cache, *client, auth, uri);
}

std::string GDriveFileSystem::DisambiguatePath(const std::string &parent_path, const DriveFileMeta &meta,
                                               bool name_is_ambiguous) {
	if (name_is_ambiguous) {
		return std::string(GDRIVE_SCHEME) + "id:" + meta.id;
	}
	return JoinGDrivePath(parent_path, meta.name);
}

unique_ptr<FileHandle> GDriveFileSystem::OpenFile(const string &path_in, FileOpenFlags flags,
                                                  optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
	if (flags.OpenForWriting() && IsDriveWebUrl(path_in)) {
		auto address = ParseDriveWebUrl(path_in);
		if (address.ok && address.is_editor) {
			// An editor address names a native Doc/Sheet/Slides file: there
			// are no bytes to replace, and uploading CSV over a Sheet is not a
			// write anyone means. Reading it works (export).
			throw InvalidInputException("gdrive: cannot write to '%s': it is a Google Docs editor address (a native "
			                            "Doc, Sheet or Slides file), which has no bytes to replace. Write a file "
			                            "instead, e.g. to a gdrive:// path.",
			                            path_in);
		}
		if (!address.ok || !address.writable) {
			// COPY stages its output at "tmp_" + the address's last segment.
			// Only in /file/d/<id>/<more> is that segment not the id itself.
			throw InvalidInputException("gdrive: cannot write to '%s': write through a file's own address, "
			                            "https://drive.google.com/file/d/<id>/view, or through gdrive://id:<id>",
			                            path_in);
		}
	}
	const string path = ToGDrivePath(path_in);
	// 'gdrive://out/' names a folder, not a file: writing it used to create a
	// FILE named "out" (the trailing slash is ignored when parsing).
	if (flags.OpenForWriting() && !path.empty() && path.back() == '/') {
		throw IOException("gdrive: cannot write to '%s': it names a folder, not a file", path_in);
	}
	auto context = RequireOpener(opener, path);
	auto parsed = ParseGDriveUri(path);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	if (!HasAnyGDriveSecret(context)) {
		throw IOException(
		    "gdrive: no gdrive secret configured for '%s' -- %s", path, kNoSecretAdvice);
	}
	auto auth = GetAuthContext(context, path);
	auto client = CreateGDriveClient(auth);

	if (flags.OpenForWriting()) {
		RefuseVersion(parsed.uri, "write");
		// Sequential, buffered write only -- nothing reaches Drive until
		// Close() (HLD section 7, R-3).
		std::string parent_id;
		std::string name;
		std::string existing_id;
		// Ids reserved for the missing part of the parent chain AND the file
		// (one files.generateIds), so every create below is retry-safe.
		std::vector<std::string> chain_ids;
		size_t next_chain_id = 0;

		if (parsed.uri.kind == GDriveUriKind::FILE_ID) {
			// One files.get: an id names an EXISTING file, and the write must
			// not land on a folder or a native Doc/Sheet.
			auto resp = client->GetMetadata(parsed.uri.file_id);
			if (!resp.ok) {
				ThrowGDriveError(resp.error, path);
			}
			DriveFileMeta existing;
			if (!ParseFileMeta(resp.body, existing)) {
				throw IOException("gdrive: malformed metadata response for '%s'", path);
			}
			if (existing.trashed) {
				throw IOException("gdrive: cannot write to '%s': the file is in the trash", path);
			}
			RefuseReplacing(existing, path);
			existing_id = parsed.uri.file_id;
		} else {
			name = parsed.uri.FileName();
			if (name.empty()) {
				throw IOException("gdrive: '%s' does not name a file to write", path);
			}

			// Walk the parent chain, creating any missing intermediate
			// folder along the way (mkdir -p semantics for the write path --
			// COPY TO a nested scratch path should not require a separate
			// CreateDirectory call first). Read-side resolution never does
			// this; only writing does.
			parent_id = RootFolderFor(auth);
			std::string parent_path_accum;
			// O-2: once a segment turns out to be missing, we CREATE it -- and
			// nothing inside a folder that did not exist a moment ago can
			// exist either. Every deeper existence probe is then answerable
			// without asking Drive. Measured on a DuckLake CTAS: five listings
			// where one is needed, each ~330 ms on the critical path.
			bool parent_is_new = false;
			for (size_t i = 0; i + 1 < parsed.uri.segments.size(); i++) {
				const auto &segment = parsed.uri.segments[i];
				parent_path_accum += (parent_path_accum.empty() ? "" : "/") + segment;

				CacheKey key {auth.identity, auth.drive_id, auth.root_folder_id, parent_path_accum};
				if (!parent_is_new) {
					DriveFileMeta cached;
					if (cache.TryGet(key, cached)) {
						// A cache hit says "this path resolves", not "this path
						// is a folder". The path cache is shared with the READ
						// side, so a file written earlier in the session is in
						// here too -- and using it as a parent sends Drive a
						// file id, which it rejects with "The specified parent
						// is not a folder": loud, but Drive's phrasing, and it
						// names neither the offending segment nor the fix.
						//
						// Do NOT throw straight from the cached value: the entry
						// may predate someone replacing that file with a folder,
						// and refusing a write on stale evidence is its own bug.
						// Drop the entry and fall through to the live check
						// below, which re-lists and reaches the right answer
						// either way. Only ever taken on the collision path, so
						// the extra round trip is not on any hot path.
						if (!cached.IsFolder()) {
							cache.InvalidatePrefix(key);
						} else {
							parent_id = cached.id;
							continue;
						}
					}
					auto matches = ListByName(*client, parent_id, segment, /*require_folder=*/true, path);
					if (matches.size() > 1) {
						ThrowAmbiguous(matches, path);
					}
					if (!matches.empty()) {
						cache.Put(key, matches[0]);
						parent_id = matches[0].id;
						continue;
					}
					// No FOLDER of that name. Before creating one, check there
					// is no FILE of that name either.
					//
					// ListByName above filters to folders, so a same-named file
					// is invisible to it -- and Drive happily holds both. Left
					// alone, this creates the folder beside the file and both
					// become unaddressable by path: the R-4 ambiguity the rest
					// of the extension refuses to manufacture. write_blob got
					// this guard first; review pointed out COPY TO comes
					// through here instead and was still poisoning paths.
					//
					// The extra listing is paid only for the FIRST missing
					// segment: once one is created, parent_is_new short-
					// circuits the rest of the chain (O-2).
					auto any_kind = ListByName(*client, parent_id, segment, /*require_folder=*/false, path);
					// This listing is unfiltered, so it can also turn up a
					// FOLDER that another writer created between the two calls.
					// Adopt it: that is the outcome we wanted anyway, and
					// reporting "a file of that name already exists" for a
					// folder would be simply untrue.
					const DriveFileMeta *existing_folder = nullptr;
					const DriveFileMeta *existing_file = nullptr;
					for (const auto &m : any_kind) {
						(m.IsFolder() ? existing_folder : existing_file) = &m;
					}
					if (existing_file) {
						throw IOException(
						    "gdrive: cannot create directory '%s' under '%s': a file of that name already "
						    "exists. Creating a folder beside it would leave two entries with one name, "
						    "which Drive allows and which would make both unaddressable by path.",
						    segment, path);
					}
					if (existing_folder) {
						cache.Put(key, *existing_folder);
						parent_id = existing_folder->id;
						continue;
					}
					// Missing entirely. Fall through and create -- and remember,
					// so the rest of the chain skips its probes.
				}

				if (chain_ids.empty()) {
					// The first missing segment. Everything below it is missing
					// too (O-2), so ONE reservation covers the rest of the chain
					// plus the file itself. Fails closed: no ids, no create.
					const auto missing_folders = parsed.uri.segments.size() - 1 - i;
					chain_ids = ReserveFileIds(*client, static_cast<int>(missing_folders + 1), path);
				}
				auto resp = client->Upload("", parent_id, segment, "application/vnd.google-apps.folder", "",
				                           chain_ids[next_chain_id++]);
				if (!resp.ok) {
					ThrowGDriveError(resp.error, path);
				}
				DriveFileMeta created;
				if (!ParseFileMeta(resp.body, created)) {
					throw IOException("gdrive: malformed metadata response creating folder for '%s'", path);
				}
				// O-3: cache the folder we just made, so a second write into
				// the same directory re-walks nothing.
				cache.Put(key, created);
				parent_id = created.id;
				parent_is_new = true;
			}

			// Overwrite semantics (S-3.9): if a file with this name already
			// exists in the (now-resolved) parent folder, remember its id so
			// Close() updates it via files.update rather than creating a
			// second file with the same name -- which would manufacture an
			// R-4 collision.
			//
			// Skipped when the parent folder was created moments ago by the
			// walk above: an empty folder cannot already contain this name, so
			// the probe has exactly one possible answer. This is the same
			// evidence O-2 uses, applied one level further down. It must NOT
			// be skipped on any other basis -- getting it wrong manufactures
			// the R-4 duplicate the probe exists to prevent.
			if (!parent_is_new) {
				auto existing_matches = ListByName(*client, parent_id, name, /*require_folder=*/false, path);
				if (existing_matches.size() > 1) {
					ThrowAmbiguous(existing_matches, path);
				} else if (existing_matches.size() == 1) {
					RefuseReplacing(existing_matches[0], path);
					existing_id = existing_matches[0].id;
				}
			}
		}

		DriveFileMeta placeholder;
		placeholder.id = existing_id;
		placeholder.name = name;
		auto handle = make_uniq<GDriveFileHandleImpl>(*this, path, flags, placeholder, auth);
		handle->is_write = true;
		handle->write_parent_id = parent_id;
		handle->write_name = name;
		if (!chain_ids.empty()) {
			// The leaf is new (its parent was just created): the last
			// reserved id is the file's.
			handle->reserved_id = chain_ids.back();
		}
		handle->upload = make_uniq<ChunkedUploadBuffer>(
		    GetUBigIntSetting(opener, "gdrive_upload_chunk_bytes", 16ULL * 1024 * 1024));
		return std::move(handle);
	}

	// Read path.
	//
	// Scope metadata caching to THIS query BEFORE resolving anything. It used
	// to happen after ResolvePath, which meant the gdrive://id: form -- whose
	// metadata lookup happens inside ResolvePath -- could serve an entry from
	// the PREVIOUS query and only then clear the cache. Stale by one query,
	// for exactly the addressing form documented as the fast path.
	{
		auto ctx = FileOpener::TryGetClientContext(opener);
		cache.BeginQuery(ctx ? ctx->transaction.GetActiveQuery() : 0);
	}
	cache.SetMaxEntries(static_cast<idx_t>(GetUBigIntSetting(opener, "gdrive_path_cache_entries", 4096)));

	// FILE_FLAGS_NULL_IF_NOT_EXISTS is part of the FileSystem contract and was
	// ignored here: core opens speculatively with it (magic-byte sniffing,
	// buffer-manager probes) and expects a null, not an exception. ONLY
	// not-found becomes null -- ambiguity (R-4), auth, quota and malformed
	// responses still throw, which is the same rule TryResolvePath follows.
	bool leaf_is_fresh = false;
	DriveFileMeta meta;
	if (!TryResolvePath(cache, *client, auth, parsed.uri, meta, &leaf_is_fresh)) {
		if (flags.ReturnNullIfNotExists()) {
			return nullptr;
		}
		throw IOException("gdrive: no such file or directory: '%s'", parsed.uri.ToString());
	}
	// A folder is not a file, and not absent either: opening one as a file
	// used to hand back a 0-byte handle, so file_size() of a folder said 0.
	if (meta.IsFolder()) {
		throw IOException("gdrive: '%s' is a directory, not a file", parsed.uri.ToString());
	}

	// The path-walk cache may serve a leaf entry captured during an earlier
	// Glob/ListFiles call; OpenFile re-fetches that leaf's own metadata fresh
	// so size and GetVersionTag (headRevisionId) reflect Drive's CURRENT state
	// (S-2.16). The FILE_ID form already did exactly this files.get inside
	// ResolvePath, so skip the duplicate call there.
	CacheKey meta_identity;
	meta_identity.identity = auth.identity;
	meta_identity.drive_id = auth.drive_id;
	meta_identity.root_folder_id = auth.root_folder_id;

	// Two ways the refresh is provably unnecessary.
	//
	// (1) The leaf was JUST listed. files.list carries the same size and
	//     headRevisionId that files.get would return -- the field mask asks
	//     for both -- so the refresh is a ~270 ms round trip that cannot
	//     differ. This is pure win and is always on. Measured: it removes the
	//     single files.get from a cold open, 0.23 s of a 3.2 s DuckLake read.
	//
	// (2) The user declared this path immutable. Then a CACHED leaf cannot be
	//     stale either, because staleness requires an in-place overwrite and
	//     the declaration says that does not happen here. This is opt-in and
	//     off by default: get it wrong and a rewritten file is read at its old
	//     size and old revision, silently. See the README's warning.
	//
	// Deletion is NOT a hazard for either: a dead file id surfaces as a 404 on
	// the first read and TryRecoverStaleHandle re-resolves. Read-time recovery
	// is strictly stronger than an open-time check anyway -- it also covers a
	// file deleted AFTER the handle was opened, which no amount of validation
	// at open can see.
	const std::string immutable_prefixes = GetStringSetting(opener, "gdrive_immutable_prefixes", "");
	const bool assume_immutable = !immutable_prefixes.empty() && PathMatchesAnyPrefix(path, immutable_prefixes);
	const bool skip_refresh = leaf_is_fresh || assume_immutable;

	if (parsed.uri.kind != GDriveUriKind::FILE_ID && !skip_refresh) {
		// Single-flight: DuckDB opens a handle per thread, so a plain
		// check-then-fetch lets every one of them miss together and issue the
		// same files.get -- the exact amplification this cache exists to
		// remove, reappearing on a cold start.
		DriveFileMeta refreshed;
		bool found = cache.GetOrFetchMetadata(meta_identity, meta.id, [&](DriveFileMeta &fresh) {
			auto resp = client->GetMetadata(meta.id);
			if (!resp.ok) {
				if (resp.error.kind == GDriveErrorKind::NOT_FOUND) {
					return false;
				}
				// Any OTHER failure (transient 5xx, rate limit) must not fail
				// an open the resolver already satisfied: fall back to what it
				// found rather than failing a read the cache thought fine.
				fresh = meta;
				return true;
			}
			if (!ParseFileMeta(resp.body, fresh)) {
				fresh = meta;
			}
			// Trashed since the path was cached: as dead as a deleted id.
			return !fresh.trashed;
		}, refreshed);

		if (found) {
			meta = refreshed;
		} else {
			// The cached id is DEAD -- another client deleted the file (and
			// perhaps recreated one with the same name, which Drive gives a
			// NEW id). Ignoring this, as we used to, opens a handle onto the
			// dead id and every read then fails "not found" for a path that
			// plainly exists, until the process restarts.
			//
			// Drop the stale entry and resolve the path again from scratch.
			// Exactly once: if the second resolve also cannot find it, the
			// file really is gone and that error is the honest answer.
			CacheKey key;
			key.identity = auth.identity;
			key.drive_id = auth.drive_id;
			key.root_folder_id = auth.root_folder_id;
			key.canonical_path = CanonicalPathOf(parsed.uri);
			cache.InvalidatePrefix(key);
			// Same flag contract as the first miss above. The cached id was
			// dead and the path is gone too, so a caller that asked for null
			// rather than a throw must still get null -- otherwise a
			// speculative core probe against a path some other client deleted
			// raises instead of reporting absence. Review caught this: the
			// first miss was handled, the re-resolve was not.
			if (!TryResolvePath(cache, *client, auth, parsed.uri, meta)) {
				if (flags.ReturnNullIfNotExists()) {
					return nullptr;
				}
				throw IOException("gdrive: no such file or directory: '%s'", parsed.uri.ToString());
			}
		}
	}

	// An earlier version: the size, time and revision are the version's, and
	// reads come from revisions.get. Asked of Drive at every open (one call):
	// a revision id is immutable, but whether Drive still keeps it is not.
	if (!parsed.uri.version.empty()) {
		if (meta.IsNativeGoogleFormat()) {
			throw InvalidInputException(
			    "gdrive: cannot read '%s': Drive keeps no downloadable content for earlier versions of Google "
			    "Docs, Sheets or Slides (only for uploaded files)",
			    parsed.uri.ToString());
		}
		auto resp = client->ApiGet("/drive/v3/files/" + QueryEncode(meta.id) + "/revisions/" +
		                               QueryEncode(parsed.uri.version) + "?fields=" + QueryEncode(RevisionFields()) +
		                               "&" + AllDrivesQuery(),
		                           &DriveCallStats::revisions_list);
		if (!resp.ok) {
			if (resp.error.kind == GDriveErrorKind::NOT_FOUND) {
				if (flags.ReturnNullIfNotExists()) {
					return nullptr;
				}
				throw IOException("gdrive: no version '%s' of '%s' (gdrive_versions() lists the versions Drive "
				                  "keeps)",
				                  parsed.uri.version, path);
			}
			ThrowGDriveError(resp.error, path);
		}
		RevisionRow revision;
		std::string error;
		if (!ParseRevision(resp.body, revision, error)) {
			throw IOException("gdrive: malformed revision response for '%s': %s", path, error);
		}
		meta.size = revision.size;
		meta.modified_time = revision.modified;
		meta.head_revision_id = revision.id;
	}

	auto handle = make_uniq<GDriveFileHandleImpl>(*this, path, flags, meta, auth);
	handle->version = parsed.uri.version;
	handle->block_size = static_cast<idx_t>(GetUBigIntSetting(opener, "gdrive_block_size_bytes", 16ULL * 1024 * 1024));
	blocks.SetCapacity(static_cast<idx_t>(GetUBigIntSetting(opener, "gdrive_block_cache_bytes", 256ULL * 1024 * 1024)));
	handle->block_key = BuildBlockKey(auth, meta);

	if (meta.IsNativeGoogleFormat()) {
		// REQ-F-07 / D-7: native Google formats have no byte stream at all;
		// alt=media cannot serve them. Export does not support Range, so the
		// whole export is fetched once here and served from the buffer.
		std::string export_mime =
		    ExportMimeTypeFor(meta, GetStringSetting(opener, "gdrive_docs_export_mime", "text/plain"));
		handle->is_export = true;
		handle->export_buffer = FetchExport(*client, meta, export_mime, path);
		handle->meta.size = static_cast<int64_t>(handle->export_buffer.size());
	}

	return std::move(handle);
	});
}

//! The block cache key for one file under one identity.
//!
//! `head_revision_id` is load-bearing: Drive keeps a file's id across an
//! overwrite, so a key without it would serve pre-overwrite bytes forever.
//! Built in ONE place because it must be rebuilt whenever `h.meta` changes --
//! it did not use to be, and a recovered handle kept reading the dead file's
//! blocks out of cache.
std::string BuildBlockKey(const GDriveAuthContext &auth, const DriveFileMeta &meta) {
	return auth.identity + '\x1f' + auth.drive_id + '\x1f' + auth.root_folder_id + '\x1f' + meta.id + '\x1f' +
	       meta.head_revision_id;
}

//! A cached file id can go dead under us: another client deletes the file and
//! recreates one at the same path, which Drive gives a NEW id.
//!
//! This used to be handled in OpenFile, by noticing that the metadata refresh
//! 404'd. Caching metadata removed that probe -- and the e2e test for it
//! failed immediately, which is the only reason this is here rather than
//! shipped broken.
//!
//! Handling it at READ time covers MORE than the version it replaces -- a file
//! deleted after the handle was opened is invisible to any validation at open
//! -- but it is not strictly better, and the earlier comment here claiming so
//! was wrong. DuckDB is told the file's size at OPEN. Recovery can redirect
//! the reads to the new id (traceable as 404 -> files.list -> 206), but it
//! cannot retract a size already reported, so a replacement of a different
//! length is read short. It converts a hard failure into a bounded read; it
//! does not make a stale open correct.
//!
//! Returns true if the handle now points at a live, different file id.
bool GDriveFileSystem::TryRecoverStaleHandle(GDriveFileHandle &base) {
	auto &h = base.Cast<GDriveFileHandleImpl>();
	if (!h.version.empty()) {
		// A version is not "the file at this path": another file there now
		// has other versions, not this one.
		return false;
	}
	auto parsed = ParseGDriveUri(h.path);
	if (!parsed.ok || parsed.uri.kind == GDriveUriKind::FILE_ID) {
		// An explicit gdrive://id: that 404s is genuinely gone. Re-resolving
		// would mean inventing a different file than the one asked for.
		return false;
	}

	CacheKey key;
	key.identity = h.auth_context.identity;
	key.drive_id = h.auth_context.drive_id;
	key.root_folder_id = h.auth_context.root_folder_id;
	key.canonical_path = CanonicalPathOf(parsed.uri);
	cache.InvalidatePrefix(key);

	auto client = CreateGDriveClient(h.auth_context);
	DriveFileMeta fresh;
	if (!TryResolvePath(cache, *client, h.auth_context, parsed.uri, fresh)) {
		return false; // really gone
	}
	if (fresh.id == h.meta.id) {
		return false; // same id -- the 404 was not staleness
	}
	h.meta = fresh;
	// The block key embeds the id AND the revision, so it MUST be rebuilt here.
	// Leaving it meant a recovered handle went on serving the dead file's
	// blocks straight out of the cache -- a stale read rather than the error
	// it replaced, which is worse than not recovering at all.
	h.block_key = BuildBlockKey(h.auth_context, h.meta);
	return true;
}

void GDriveFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	if (nr_bytes == 0) {
		return;
	}

	if (h.is_export) {
		// Export has no Range support; served from the cached buffer.
		idx_t avail = h.export_buffer.size() > location ? h.export_buffer.size() - location : 0;
		idx_t to_copy = MinValue<idx_t>(static_cast<idx_t>(nr_bytes), avail);
		if (to_copy > 0) {
			memcpy(buffer, h.export_buffer.data() + location, to_copy);
		}
		if (to_copy < static_cast<idx_t>(nr_bytes)) {
			throw IOException("gdrive: short read past end of exported content for '%s'", h.path);
		}
		// NOTE: does NOT touch h.position -- see the comment below.
		return;
	}

	// Guard the range arithmetic before it can wrap. `location` is idx_t
	// (unsigned) and the Range header is built from int64_t, so a location
	// past INT64_MAX -- or a location+length that crosses it -- would produce
	// a negative range and a baffling Drive error instead of a clear one.
	// Drive files cannot actually be this large, which is exactly why this
	// would only ever fire on a caller bug, and should say so.
	constexpr int64_t kMaxOffset = std::numeric_limits<int64_t>::max();
	if (location > static_cast<idx_t>(kMaxOffset) || nr_bytes < 0 ||
	    static_cast<int64_t>(location) > kMaxOffset - nr_bytes) {
		throw IOException("gdrive: read range out of bounds for '%s': offset %llu length %lld", h.path,
		                  static_cast<unsigned long long>(location), static_cast<long long>(nr_bytes));
	}

	// -----------------------------------------------------------------------
	// Block cache. Drive charges ~1.2 s per media request regardless of size,
	// so serving a scan from a few large blocks beats many small ranged GETs
	// even though it transfers more. See GDriveBlockCache.
	// -----------------------------------------------------------------------
	if (h.block_size > 0 && h.meta.size > 0) {
		// One retry, for the same reason the exact-range path below has one:
		// the cached file id can be dead. This path used to have NO recovery at
		// all, so on the DEFAULT configuration (16 MiB blocks) a
		// delete-and-recreate under a live handle failed hard while the
		// fallback path -- reachable only with the block cache switched off --
		// recovered cleanly. Found in review, not by a test, because every
		// existing stale-id test happened to exercise the open-time refresh
		// instead.
		//
		// `not_found` is captured rather than inferred from the exception:
		// ThrowGDriveError erases GDriveErrorKind into a DuckDB exception type,
		// and matching on the message text would be worse than useless.
		for (int attempt = 0;; attempt++) {
			bool not_found = false;
			try {
				ReadViaBlockCache(h, buffer, nr_bytes, location, not_found);
				return;
			} catch (...) {
				if (attempt == 0 && not_found && TryRecoverStaleHandle(h)) {
					// TryRecoverStaleHandle rebuilds block_key, so the retry
					// reads the NEW file rather than the dead one's blocks.
					continue;
				}
				throw;
			}
		}
	}

	auto client = CreateGDriveClient(h.auth_context);
	// Inclusive end, as HTTP Range and Drive both define it.
	int64_t end = static_cast<int64_t>(location) + nr_bytes - 1;

	auto resp = DownloadFor(*client, h, static_cast<int64_t>(location), end);
	if (!resp.ok && resp.error.kind == GDriveErrorKind::NOT_FOUND && TryRecoverStaleHandle(h)) {
		// The path still exists, under a new id. Retry ONCE against it: a
		// loop here would spin against a file being rewritten repeatedly.
		client = CreateGDriveClient(h.auth_context);
		resp = DownloadFor(*client, h, static_cast<int64_t>(location), end);
	}
	if (!resp.ok) {
		ThrowGDriveError(resp.error, h.path);
	}
	ReadExactRangeTail(h.path, buffer, nr_bytes, location, resp);

	// Deliberately does NOT advance h.position.
	//
	// This overload is pread(2): it takes an explicit offset and must not
	// disturb the shared cursor. DuckDB's parallel Parquet reader issues
	// positional reads for different row groups against ONE handle from
	// several threads at once, so writing h.position here was both a data
	// race and semantically wrong. The sequential overload owns the cursor.
}

//! The block-cache read, factored out so Read() can wrap it in one retry.
//! Sets `not_found` if a fetch failed specifically with NOT_FOUND, before
//! rethrowing -- see the caller for why the flag rather than the exception.
void GDriveFileSystem::ReadViaBlockCache(GDriveFileHandle &base, void *buffer, int64_t nr_bytes, idx_t location,
                                          bool &not_found) {
	auto &h = base.Cast<GDriveFileHandleImpl>();
	{
		auto client_for_block = CreateGDriveClient(h.auth_context);
		idx_t remaining = static_cast<idx_t>(nr_bytes);
		idx_t out_offset = 0;
		idx_t pos = location;
		while (remaining > 0) {
			idx_t block_index = pos / h.block_size;
			idx_t block_start = block_index * h.block_size;
			auto block = blocks.GetBlock(
			    h.block_key, block_index, h.block_size, static_cast<idx_t>(h.meta.size),
			    [&](idx_t start, idx_t len, std::string &out) {
				    auto r = DownloadFor(*client_for_block, h, static_cast<int64_t>(start),
				                         static_cast<int64_t>(start + len - 1));
				    if (!r.ok) {
					    if (r.error.kind == GDriveErrorKind::NOT_FOUND) {
						    not_found = true;
					    }
					    ThrowGDriveError(r.error, h.path);
				    }
				    // A 200 means the server IGNORED the Range and sent the
				    // whole file; only a 206 body starts at `start`. The
				    // exact-read path below has handled this since the bug was
				    // found -- caching the un-normalised body here would have
				    // reintroduced it, and made it worse by persisting the
				    // wrong bytes for every later reader of the block.
				    if (r.http_status == 200 && start > 0) {
					    if (r.body.size() <= start) {
						    throw IOException("gdrive: server ignored Range on '%s' and returned %llu bytes, "
						                      "which does not reach offset %llu",
						                      h.path, static_cast<unsigned long long>(r.body.size()),
						                      static_cast<unsigned long long>(start));
					    }
					    out = r.body.substr(start, len);
				    } else {
					    out = std::move(r.body);
				    }
			    });
			idx_t within = pos - block_start;
			if (within >= block->size()) {
				throw IOException("gdrive: short read on '%s': block %llu ended at %llu, wanted offset %llu", h.path,
				                  static_cast<unsigned long long>(block_index),
				                  static_cast<unsigned long long>(block->size()),
				                  static_cast<unsigned long long>(within));
			}
			idx_t take = MinValue<idx_t>(remaining, block->size() - within);
			memcpy(static_cast<char *>(buffer) + out_offset, block->data() + within, take);
			remaining -= take;
			out_offset += take;
			pos += take;
		}
	}
}

namespace {

//! Copy an exact-range response into the caller's buffer, applying the
//! 200-vs-206 offset. A free function: it needs no filesystem state, and
//! taking the path as a string keeps it off GDriveFileHandleImpl entirely.
void ReadExactRangeTail(const std::string &path, void *buffer, int64_t nr_bytes, idx_t location,
                        const DriveResponse &resp) {
	// A 206 body starts at `location`. A 200 body is the WHOLE FILE -- the
	// server (or an intermediary proxy) ignored the Range header, which it is
	// permitted to do. Download() accepts both by design, so the offset must
	// be applied here.
	//
	// Getting this wrong is silent: copying from body.data() for a 200 hands
	// back bytes 0..n from the start of the file while the caller believes it
	// asked for bytes at `location`. Every value is plausible, nothing errors,
	// and a Parquet scan just returns wrong data.
	const char *src = resp.body.data();
	size_t avail = resp.body.size();
	if (resp.http_status == 200 && location > 0) {
		if (avail <= static_cast<size_t>(location)) {
			throw IOException("gdrive: server ignored Range on '%s' and returned %llu bytes, "
			                  "which does not reach offset %llu",
			                  path, static_cast<unsigned long long>(avail),
			                  static_cast<unsigned long long>(location));
		}
		src += location;
		avail -= static_cast<size_t>(location);
	}

	if (avail < static_cast<size_t>(nr_bytes)) {
		throw IOException("gdrive: short read on '%s': requested %lld bytes at offset %llu, got %llu",
		                  path, static_cast<long long>(nr_bytes), static_cast<unsigned long long>(location),
		                  static_cast<unsigned long long>(avail));
	}
	memcpy(buffer, src, static_cast<size_t>(nr_bytes));
}

} // namespace

int64_t GDriveFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	idx_t remaining = static_cast<idx_t>(h.meta.size) > h.position ? static_cast<idx_t>(h.meta.size) - h.position : 0;
	idx_t to_read = MinValue<idx_t>(remaining, static_cast<idx_t>(nr_bytes));
	if (to_read == 0) {
		return 0;
	}
	idx_t at = h.position;
	Read(handle, buffer, static_cast<int64_t>(to_read), at);
	// The sequential overload is the only writer of the cursor.
	h.position = at + to_read;
	return static_cast<int64_t>(to_read);
}

void GDriveFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw NotImplementedException(
	    "gdrive: cannot write at a byte offset -- Google Drive has no positional write API. "
	    "Write sequentially and close the file; it is streamed in chunks and published on close.");
}

int64_t GDriveFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	if (!h.is_write || !h.upload) {
		throw IOException("gdrive: handle for '%s' was not opened for writing", h.path);
	}
	if (h.write_failed) {
		throw IOException("gdrive: a previous write to '%s' already failed; this handle can only be closed", h.path);
	}
	auto &buf = *h.upload;
	const char *data = reinterpret_cast<const char *>(buffer);
	idx_t left = static_cast<idx_t>(nr_bytes);
	try {
		while (left > 0) {
			// A full chunk goes up only once MORE bytes arrive: until then it
			// may be the whole file (one multipart request) or the final chunk
			// (which must carry the total). So the buffer never exceeds one
			// chunk, whatever size this Write() is.
			if (buf.Full()) {
				SendUploadChunk(h);
			}
			const auto took = buf.Fill(data, left);
			data += took;
			left -= took;
		}
	} catch (...) {
		// The stream is broken: mark the write failed so Close() refuses to
		// publish a possibly-truncated file (R-3), and cancel the session.
		h.write_failed = true;
		AbortUpload(h);
		throw;
	}
	RecordGlobalUploadBufferPeak(buf.Peak());
	h.position += nr_bytes;
	return nr_bytes;
}

int64_t GDriveFileSystem::GetFileSize(FileHandle &handle) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	return h.meta.size < 0 ? 0 : h.meta.size;
}

timestamp_t GDriveFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	return ParseRfc3339(h.meta.modified_time);
}

std::string GDriveFileSystem::GetVersionTag(FileHandle &handle) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	return h.meta.head_revision_id;
}

// ---------------------------------------------------------------------------
// Existence checks.
//
// Only NOT-FOUND may become `false`. An R-4 ambiguity, a 403, an expired
// token or a malformed response must propagate.
//
// These used to `catch (...) { return false; }`, which meant a path with two
// files of the same name -- and a path we simply had no permission to see --
// both reported "does not exist". A caller using existence as a guard then
// proceeds as though the path is free, which for a write is how you get a
// third duplicate.
//
// TryResolvePath already draws exactly this line (see its comment: Glob's
// not-found-is-empty semantics must swallow only not-found and NEVER an auth
// failure). These functions simply were not using it. Same discipline, one
// layer up.
// ---------------------------------------------------------------------------
bool GDriveFileSystem::FileExists(const string &filename_in, optional_ptr<FileOpener> opener) {
	string filename;
	if (!TryToGDrivePath(filename_in, filename)) {
		return false;
	}
	auto context = RequireOpener(opener, filename);
	if (!HasAnyGDriveSecret(context)) {
		// No gdrive secret at all: nothing to check against, and erroring here
		// would break DuckDB probing a path it is merely considering.
		return false;
	}
	auto parsed = ParseGDriveUri(filename);
	if (!parsed.ok) {
		return false;
	}
	auto auth = GetAuthContext(context, filename);
	auto client = CreateGDriveClient(auth);
	DriveFileMeta meta;
	if (!TryResolvePath(cache, *client, auth, parsed.uri, meta)) {
		return false; // genuinely absent -- the ONLY false-worthy outcome
	}
	return !meta.IsFolder();
}

bool GDriveFileSystem::DirectoryExists(const string &directory_in, optional_ptr<FileOpener> opener) {
	string directory;
	if (!TryToGDrivePath(directory_in, directory)) {
		return false;
	}
	auto context = RequireOpener(opener, directory);
	if (!HasAnyGDriveSecret(context)) {
		return false;
	}
	auto parsed = ParseGDriveUri(directory);
	if (!parsed.ok) {
		return false;
	}
	if (parsed.uri.kind == GDriveUriKind::PATH && parsed.uri.segments.empty()) {
		return true; // the configured root always "exists"
	}
	auto auth = GetAuthContext(context, directory);
	auto client = CreateGDriveClient(auth);
	DriveFileMeta meta;
	if (!TryResolvePath(cache, *client, auth, parsed.uri, meta)) {
		return false;
	}
	return meta.IsFolder();
}

void GDriveFileSystem::CreateDirectory(const string &directory_in, optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
	const string directory = ToGDrivePath(directory_in);
	auto context = RequireOpener(opener, directory);
	auto parsed = ParseGDriveUri(directory);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	RefuseVersion(parsed.uri, "create a directory at");
	auto auth = GetAuthContext(context, directory);
	auto client = CreateGDriveClient(auth);
	MutateCreateDirectory(*client, cache, auth, parsed.uri);
	});
}

void GDriveFileSystem::RemoveDirectory(const string &directory_in, optional_ptr<FileOpener> opener) {
	const string directory = ToGDrivePath(directory_in);
	auto context = RequireOpener(opener, directory);
	auto parsed = ParseGDriveUri(directory);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	RefuseVersion(parsed.uri, "remove");
	auto auth = GetAuthContext(context, directory);
	auto client = CreateGDriveClient(auth);
	bool permanent = GetBoolSetting(opener, "gdrive_permanent_delete", false);
	MutateRemoveDirectory(*client, cache, auth, parsed.uri, permanent);
}

bool GDriveFileSystem::ListFiles(const string &directory_in, const std::function<void(const string &, bool)> &callback,
                                 FileOpener *opener) {
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
	string directory;
	if (!TryToGDrivePath(directory_in, directory)) {
		return false;
	}
	optional_ptr<FileOpener> op(opener);
	auto context = RequireOpener(op, directory);
	if (!HasAnyGDriveSecret(context)) {
		return false;
	}
	auto parsed = ParseGDriveUri(directory);
	if (!parsed.ok) {
		return false;
	}
	auto auth = GetAuthContext(context, directory);
	auto client = CreateGDriveClient(auth);

	std::string parent_id;
	if (parsed.uri.kind == GDriveUriKind::PATH && parsed.uri.segments.empty()) {
		parent_id = RootFolderFor(auth);
	} else {
		auto dir_meta = ResolvePath(cache, *client, auth, parsed.uri);
		if (!dir_meta.IsFolder()) {
			throw IOException("gdrive: '%s' is not a directory", directory);
		}
		parent_id = dir_meta.id;
	}

	auto children = ListChildren(*client, parent_id, directory);

	// R-4 applies to listing too (S-2.8): two siblings sharing a name are
	// indistinguishable as plain names. NOTE: ListFiles' callback contract is
	// a bare child name (the caller joins it with `directory`), unlike Glob's
	// full gdrive:// paths, so the id: substitution here is a best-effort
	// bare-name form ("id:<fileId>") rather than the full "gdrive://id:..."
	// address -- callers that need an unambiguous, directly-openable path
	// should prefer Glob.
	unordered_map<std::string, int> name_counts;
	for (auto &child : children) {
		name_counts[child.name]++;
	}
	for (auto &child : children) {
		bool ambiguous = name_counts[child.name] > 1;
		std::string name = ambiguous ? ("id:" + child.id) : child.name;
		callback(name, child.IsFolder());
	}
	return true;
	});
}

void GDriveFileSystem::MoveFile(const string &source_in, const string &target_in, optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
	const string source = ToGDrivePath(source_in);
	const string target = ToGDrivePath(target_in);
	// COPY ... TO '<browser address>' stages a "tmp_" sibling and moves it
	// onto the target -- but an address names a file by id, so both are the
	// same id and the write already went to the file itself. A move onto
	// itself is a no-op; carried out it would delete the file it replaced.
	if (source == target) {
		return;
	}
	auto context = RequireOpener(opener, source);
	auto src_parsed = ParseGDriveUri(source);
	if (!src_parsed.ok) {
		throw IOException("gdrive: %s", src_parsed.error);
	}
	auto dst_parsed = ParseGDriveUri(target);
	if (!dst_parsed.ok) {
		throw IOException("gdrive: %s", dst_parsed.error);
	}
	RefuseVersion(src_parsed.uri, "move");
	RefuseVersion(dst_parsed.uri, "move onto");
	auto auth = GetAuthContext(context, source);
	auto client = CreateGDriveClient(auth);
	bool permanent = GetBoolSetting(opener, "gdrive_permanent_delete", false);
	MutateMoveFile(*client, cache, auth, src_parsed.uri, dst_parsed.uri, permanent);
	});
}

void GDriveFileSystem::RemoveFile(const string &filename_in, optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
	const string filename = ToGDrivePath(filename_in);
	auto context = RequireOpener(opener, filename);
	auto parsed = ParseGDriveUri(filename);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	RefuseVersion(parsed.uri, "remove");
	auto auth = GetAuthContext(context, filename);
	auto client = CreateGDriveClient(auth);
	bool permanent = GetBoolSetting(opener, "gdrive_permanent_delete", false);
	MutateRemoveFile(*client, cache, auth, parsed.uri, permanent);
	});
}

bool GDriveFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	if (IsDriveWebUrl(filename) || IsCopyStagingAlias(filename)) {
		return false;
	}
	return FileSystem::TryRemoveFile(filename, opener);
}

vector<OpenFileInfo> GDriveFileSystem::Glob(const string &path, FileOpener *opener) {
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		// A browser address is one file or folder by id: glob() hands back the
		// address itself, so a reader opens what the user wrote.
		if (IsDriveWebUrl(path)) {
			auto results = GlobImpl(ToGDrivePath(path), opener, "", nullptr);
			for (auto &r : results) {
				r.path = path;
			}
			return results;
		}
		return GlobImpl(path, opener, "", nullptr);
	});
}

vector<OpenFileInfo> GDriveFileSystem::GlobImpl(const string &path, optional_ptr<FileOpener> op,
                                                const std::string &file_fields, std::vector<GDriveItem> *items,
                                                bool include_folders) {
	// Each match, recorded for ResolveItems as it is added to the result.
	auto add_item = [items](const std::string &match_path, const DriveFileMeta &meta, const FileRow *row) {
		if (!items) {
			return;
		}
		GDriveItem item;
		item.path = match_path;
		item.meta = meta;
		if (row) {
			item.row = *row;
			item.has_row = true;
		}
		items->push_back(std::move(item));
	};
	{
	auto context = RequireOpener(op, path);
	// Deliberately NOT swallowed into "no matches": a missing secret is a
	// configuration error, not an empty result, and reporting it as zero
	// rows is exactly the misleading-failure pattern R-2/REQ-F-08 exist to
	// prevent, one layer up from where those requirements originally target.
	if (!HasAnyGDriveSecret(context)) {
		throw IOException(
		    "gdrive: no gdrive secret configured for '%s' -- %s", path, kNoSecretAdvice);
	}
	// Scope the metadata cache to THIS query before resolving anything, the
	// same as OpenFile. Glob's literal-path probe below does a full resolve
	// (including a files.get for the id: form), and without this it would
	// neither be served from nor warm the query's cache -- so a glob followed
	// by an open of the same file paid the metadata round trip twice.
	{
		auto ctx = FileOpener::TryGetClientContext(op);
		cache.BeginQuery(ctx ? ctx->transaction.GetActiveQuery() : 0);
	}
	cache.SetMaxEntries(static_cast<idx_t>(GetUBigIntSetting(op, "gdrive_path_cache_entries", 4096)));

	auto parsed = ParseGDriveUri(path);
	if (!parsed.ok) {
		if (IsFileIdFallbackGlobProbe(path)) {
			// DuckDB core's FALLBACK_GLOB directory-probe heuristic
			// (FileSystem::GlobFileList) retries a literal path that
			// resolved to zero matches by appending "/**/*.<ext>", on the
			// generic assumption that the path might be a directory. A
			// gdrive://id:<fileId> address is never a directory, so the
			// honest answer here is "no matches" -- not M-5's typo
			// rejection, which is reserved for shapes a user actually
			// wrote by hand (see gdrive_uri.hpp's doc comment on this
			// helper for exactly why the match must be this narrow).
			return {};
		}
		throw IOException("gdrive: %s", parsed.error);
	}

	// No metacharacters: resolve directly rather than listing a whole folder
	// (that is the entire point of HasGlobMetacharacters -- one call instead
	// of a listing). Glob semantics: not-found is an empty result, but ONLY
	// not-found -- ambiguity (R-4), auth failures and other API errors must
	// still surface, so this uses TryResolvePath rather than a broad catch.
	// A version path is one file: its '?' is not a glob character.
	if (parsed.uri.kind == GDriveUriKind::FILE_ID || !parsed.uri.version.empty() || !HasGlobMetacharacters(path)) {
		vector<OpenFileInfo> result;
		auto auth = GetAuthContext(context, path);
		auto client = CreateGDriveClient(auth);
		DriveFileMeta meta;
		// Folders are not glob results. The listing branch below already
		// filters them; this branch did not, so `glob('gdrive://a-folder')`
		// -- a literal path, no metacharacters -- handed a folder back as if
		// it were a file, and the caller fed it to read_parquet. Every other
		// filesystem's glob yields files only.
		if (TryResolvePath(cache, *client, auth, parsed.uri, meta) && !meta.IsFolder()) {
			result.emplace_back(path);
			add_item(path, meta, nullptr);
		}
		return result;
	}

	auto auth = GetAuthContext(context, path);
	auto client = CreateGDriveClient(auth);

	// The pure glob helpers work on bare (scheme-stripped) paths.
	std::string bare = path.substr(std::strlen(GDRIVE_SCHEME));

	auto expanded = ExpandBraces(bare);
	if (expanded.empty()) {
		// CALLER OBLIGATION (gdrive_glob.hpp): an empty result from
		// ExpandBraces means "pattern rejected, too large to expand", NOT
		// "zero matches". Treating it as no-matches would silently drop
		// every file the user asked for.
		throw InvalidInputException(
		    "gdrive: glob pattern '%s' is too large to expand (brace-alternation overflow)", path);
	}

	vector<OpenFileInfo> result;
	for (auto &one_pattern : expanded) {
		// An expansion may be a plain literal path ("{a,b}.txt" -> "a.txt").
		// SplitGlob would make that whole path the "literal prefix" and list
		// its children -- a file has none, so it never matched. Resolve it
		// directly instead, like the no-metacharacter path above.
		if (!HasGlobMetacharacters(one_pattern)) {
			auto literal = ParseGDriveUri(std::string(GDRIVE_SCHEME) + one_pattern);
			DriveFileMeta meta;
			if (literal.ok && TryResolvePath(cache, *client, auth, literal.uri, meta) && !meta.IsFolder()) {
				result.emplace_back(std::string(GDRIVE_SCHEME) + one_pattern);
				add_item(std::string(GDRIVE_SCHEME) + one_pattern, meta, nullptr);
			}
			continue;
		}
		auto split = SplitGlob(one_pattern);

		std::string parent_id;
		if (split.literal_prefix.empty()) {
			parent_id = RootFolderFor(auth);
		} else {
			auto prefix_parsed = ParseGDriveUri(std::string(GDRIVE_SCHEME) + split.literal_prefix);
			if (!prefix_parsed.ok) {
				throw IOException("gdrive: %s", prefix_parsed.error);
			}
			DriveFileMeta prefix_meta;
			if (!TryResolvePath(cache, *client, auth, prefix_parsed.uri, prefix_meta)) {
				continue; // the literal prefix doesn't exist: no matches under it
			}
			parent_id = prefix_meta.id;
		}

		// List (recursively when the tail spans segments) and match locally
		// -- Drive's API cannot glob. `listing` maps a path relative to
		// split.literal_prefix to its metadata.
		std::vector<std::pair<std::string, DriveFileMeta>> listing;
		// Parallel to `listing` when the caller asked for rows (file_fields).
		std::vector<FileRow> listing_rows;
		const bool rich = !file_fields.empty();
		std::function<void(const std::string &, const std::string &)> list_folder;
		list_folder = [&](const std::string &folder_id, const std::string &rel_prefix) {
			std::vector<FileRow> child_rows;
			auto children = ListChildren(*client, folder_id, path, file_fields, rich ? &child_rows : nullptr);
			for (size_t c = 0; c < children.size(); c++) {
				auto &child = children[c];
				std::string rel = rel_prefix.empty() ? child.name : (rel_prefix + "/" + child.name);
				listing.emplace_back(rel, child);
				if (rich) {
					listing_rows.push_back(child_rows[c]);
				}
				if (split.recursive && child.IsFolder()) {
					list_folder(child.id, rel);
				}
			}
		};
		list_folder(parent_id, "");

		// O-1: cache what we just listed.
		//
		// Without this the listing is matched, returned, and thrown away, and
		// DuckDB's multi-file reader then opens each match -- re-resolving
		// every leaf with its own files.list. Measured: 151 listings to read
		// 150 files, when all 150 came back in the first response, complete
		// with the size and headRevisionId that OpenFile needs.
		//
		// ONLY unambiguous names. Drive allows two files to share a name in
		// one folder; the loop below already detects that for DisambiguatePath.
		// Caching one of a duplicate pair would make a later OpenFile silently
		// pick it instead of raising the R-4 ambiguity error -- turning a
		// loud, correct failure into results that depend on Drive's ordering.
		{
			std::unordered_map<std::string, int> rel_counts;
			for (auto &entry : listing) {
				rel_counts[entry.first]++;
			}
			// An ambiguous ANCESTOR poisons everything beneath it. With a
			// recursive `**`, two folders sharing a name each contribute their
			// own children, whose relative paths are individually unique -- so
			// counting exact paths alone would cache `dup/only_a.csv` and
			// `dup/only_b.csv` even though `dup` names two different folders.
			//
			// Today that is latent rather than live: TryResolvePath walks
			// segment by segment, so it resolves `dup`, hits the R-4 ambiguity
			// and throws long before the leaf entry is consulted. A review
			// flagged it as an active bug; it is not, and a test
			// (e2e/tests/test_glob_cache.py) confirms the error is raised with
			// this guard removed.
			//
			// It is kept anyway, because the entry is simply WRONG DATA: it
			// asserts that one path names one file when it names two. Any
			// future fast path that resolves a full path without walking its
			// ancestors -- exactly what a name-search optimisation would do --
			// turns wrong data into a silently wrong answer. Cheap here (the
			// ambiguous list is empty in every normal case), and it removes a
			// trap from under the next optimisation.
			std::vector<std::string> ambiguous_prefixes;
			for (auto &kv : rel_counts) {
				if (kv.second != 1) {
					ambiguous_prefixes.push_back(kv.first);
				}
			}
			auto under_ambiguous = [&ambiguous_prefixes](const std::string &rel) {
				for (auto &amb : ambiguous_prefixes) {
					if (rel.size() > amb.size() && rel.compare(0, amb.size(), amb) == 0 &&
					    rel[amb.size()] == '/') {
						return true;
					}
				}
				return false;
			};
			for (auto &entry : listing) {
				if (rel_counts[entry.first] != 1 || under_ambiguous(entry.first)) {
					continue;
				}
				std::string full_rel = split.literal_prefix.empty()
				                           ? entry.first
				                           : (split.literal_prefix + "/" + entry.first);
				// Must match CanonicalPathOf's form exactly, or this is a
				// cache that can never be hit.
				CacheKey key {auth.identity, auth.drive_id, auth.root_folder_id, full_rel};
				cache.Put(key, entry.second);
			}
		}

		for (auto &entry : listing) {
			const std::string &rel_path = entry.first;
			const DriveFileMeta &meta = entry.second;
			std::string full_rel_path = split.literal_prefix.empty() ? rel_path : (split.literal_prefix + "/" + rel_path);
			if (!MatchPath(one_pattern, full_rel_path)) {
				continue;
			}
			// Folders are traversed but never RETURNED, matching what
			// glob() does on every other filesystem: DuckDB's glob is a
			// file-listing function feeding read_parquet/read_csv, and the
			// local implementation yields only files. Emitting folders here
			// made `gdrive://` the odd one out, so a caller that globbed a
			// tree and read each hit got a folder handed to it as if it
			// were a file. They are still listed above (recursion needs
			// them) and still cached (path resolution needs them) -- only
			// the result set excludes them.
			if (meta.IsFolder()) {
				// Not a glob result -- but an audit of "who can reach what"
				// must see a folder shared with anyone, not only its files.
				if (include_folders) {
					bool ambiguous = false;
					for (auto &other : listing) {
						ambiguous = ambiguous || (&other != &entry && other.first == rel_path);
					}
					auto slash = full_rel_path.find_last_of('/');
					add_item(DisambiguatePath(slash == std::string::npos ? "" : full_rel_path.substr(0, slash), meta,
					                          ambiguous),
					         meta, rich ? &listing_rows[&entry - listing.data()] : nullptr);
				}
				continue;
			}

			// R-4 for listings (S-2.8): an entry is ambiguous when another
			// listed entry shares its exact relative path (same folder, same
			// name).
			bool ambiguous = false;
			for (auto &other : listing) {
				if (&other != &entry && other.first == rel_path) {
					ambiguous = true;
					break;
				}
			}

			auto slash = full_rel_path.find_last_of('/');
			std::string entry_parent = (slash == std::string::npos) ? "" : full_rel_path.substr(0, slash);
			result.emplace_back(DisambiguatePath(entry_parent, meta, ambiguous));
			add_item(result.back().path, meta, rich ? &listing_rows[&entry - listing.data()] : nullptr);
		}
	}
	// Overlapping {a,b} expansions ("{a,a}.csv", "{x,*}.csv") must not return a
	// file twice -- DuckDB's multi-file readers would read it twice.
	{
		std::unordered_set<std::string> seen;
		vector<OpenFileInfo> unique;
		for (auto &info : result) {
			if (seen.insert(info.path).second) {
				unique.push_back(std::move(info));
			}
		}
		result = std::move(unique);
	}
	// Names may legitimately contain glob characters: "report[1].csv" is a
	// valid Drive name, but '[1]' as a pattern matches "report1.csv". When the
	// pattern matched nothing, try it as a literal path -- DuckDB's local
	// filesystem does the same -- or such a file could never be read by its
	// own name. One resolve, only on an empty match; ambiguity and API errors
	// still surface (TryResolvePath returns false only for not-found).
	if (result.empty()) {
		DriveFileMeta meta;
		if (TryResolvePath(cache, *client, auth, parsed.uri, meta) && !meta.IsFolder()) {
			result.emplace_back(path);
			add_item(path, meta, nullptr);
		}
	}
	return result;
	}
}

namespace {
//! files.get with the FileRow fields: a literal match's row, or one Glob
//! resolved without a listing (a brace-literal, the literal fallback).
FileRow FetchFileRow(GDriveClient &client, const std::string &file_id, const std::string &path) {
	auto resp = client.ApiGet("/drive/v3/files/" + QueryEncode(file_id) + "?fields=" + QueryEncode(FileRowFields()) +
	                              "&" + AllDrivesQuery(),
	                          &DriveCallStats::files_get);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, path);
	}
	FileRow row;
	std::string error;
	if (!ParseFileRow(resp.body, row, error)) {
		throw IOException("gdrive: malformed metadata response for '%s': %s", path, error);
	}
	return row;
}
} // namespace

std::vector<GDriveItem> GDriveFileSystem::ResolveItems(const string &path_in, optional_ptr<FileOpener> opener,
                                                       bool rich, bool folder_as_item) {
	const string path = ToGDrivePath(path_in);
	auto parsed = ParseGDriveUri(path);
	if (!parsed.ok) {
		throw InvalidInputException("gdrive: %s", parsed.error);
	}
	// The metadata functions describe a file; a version of one has none of
	// its own to describe (read_gdrive_files used to report the CURRENT
	// revision's size and md5 under a ?version= path).
	if (!parsed.uri.version.empty()) {
		throw InvalidInputException("gdrive: name the file, not one of its versions: '%s' (gdrive_versions() lists "
		                            "them)",
		                            path);
	}
	std::vector<GDriveItem> items;
	const bool literal = parsed.uri.kind == GDriveUriKind::FILE_ID || !HasGlobMetacharacters(path);
	if (!literal) {
		GlobImpl(path, opener, rich ? FileRowFields() : "", &items, /*include_folders=*/folder_as_item);
		// Overlapping {a,b} expansions: Glob dedupes its paths, and so must
		// this -- keeping a match that came with its row over one without.
		std::unordered_map<std::string, size_t> at;
		std::vector<GDriveItem> unique;
		for (auto &item : items) {
			auto hit = at.find(item.path);
			if (hit == at.end()) {
				at.emplace(item.path, unique.size());
				unique.push_back(std::move(item));
			} else if (!unique[hit->second].has_row && item.has_row) {
				unique[hit->second] = std::move(item);
			}
		}
		// A brace-literal ("{a,b}.csv") or the literal fallback is resolved
		// directly, with no listing to carry the row: fetch it, one files.get
		// each. Without this those rows came back blank (found in review).
		if (rich) {
			std::unique_ptr<GDriveClient> client;
			for (auto &item : unique) {
				if (item.has_row) {
					continue;
				}
				if (!client) {
					client = CreateGDriveClient(GetAuthContext(RequireOpener(opener, path), path));
				}
				item.row = FetchFileRow(*client, item.meta.id, item.path);
				item.has_row = true;
			}
		}
		return unique;
	}

	// A literal path: resolved directly, folders included -- the table
	// functions take a folder where Glob() never returns one.
	auto context = RequireOpener(opener, path);
	if (!HasAnyGDriveSecret(context)) {
		throw IOException(
		    "gdrive: no gdrive secret configured for '%s' -- %s", path, kNoSecretAdvice);
	}
	{
		auto ctx = FileOpener::TryGetClientContext(opener);
		cache.BeginQuery(ctx ? ctx->transaction.GetActiveQuery() : 0);
	}
	cache.SetMaxEntries(static_cast<idx_t>(GetUBigIntSetting(opener, "gdrive_path_cache_entries", 4096)));
	auto auth = GetAuthContext(context, path);
	auto client = CreateGDriveClient(auth);
	DriveFileMeta meta;
	if (!TryResolvePath(cache, *client, auth, parsed.uri, meta)) {
		// A literal path that names nothing is an error, as in the sibling
		// extension: returning no rows made a typo read as "nothing is shared"
		// in a permissions audit. Only a glob may match nothing.
		throw IOException("gdrive: no such file or folder: '%s'", path);
	}

	if (meta.IsFolder() && !folder_as_item) {
		// The files directly in the folder: one listing, rows included.
		std::vector<FileRow> rows;
		auto children = ListChildren(*client, meta.id, path, rich ? FileRowFields() : "", rich ? &rows : nullptr);
		std::unordered_map<std::string, int> name_counts;
		for (auto &child : children) {
			name_counts[child.name]++;
		}
		const auto parent_path = CanonicalPathOf(parsed.uri);
		for (size_t i = 0; i < children.size(); i++) {
			if (children[i].IsFolder()) {
				continue;
			}
			GDriveItem item;
			// Under an id: folder there is no path to join onto, so each child
			// is addressed by its own id.
			const bool by_id = parsed.uri.kind == GDriveUriKind::FILE_ID || name_counts[children[i].name] > 1;
			item.path = DisambiguatePath(parent_path, children[i], by_id);
			item.meta = children[i];
			if (rich) {
				item.row = rows[i];
				item.has_row = true;
			}
			items.push_back(std::move(item));
		}
		return items;
	}

	GDriveItem item;
	item.path = path;
	item.meta = meta;
	if (rich) {
		item.row = FetchFileRow(*client, meta.id, path);
		item.has_row = true;
	}
	items.push_back(std::move(item));
	return items;
}

void GDriveFileSystem::AbortFileWrite(FileHandle &handle) {
	handle.Cast<GDriveFileHandle>().CancelWrite();
}

void GDriveFileSystem::FileSync(FileHandle &handle) {
	// Full chunks already went up as they filled; the partial one cannot (a
	// non-final chunk must be a 256-KiB multiple, and Drive publishes the file
	// only on the final chunk in Close()). So this is a deliberate no-op
	// rather than a throw -- callers (e.g. a Parquet writer) that call
	// FileSync mid-write should not fail on a filesystem that simply defers
	// everything to Close().
}

void GDriveFileSystem::Seek(FileHandle &handle, idx_t location) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	h.position = location;
}

idx_t GDriveFileSystem::SeekPosition(FileHandle &handle) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	return h.position;
}

void GDriveFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	throw NotImplementedException("gdrive: cannot truncate in place -- Google Drive has no partial-content API. "
	                              "Rewrite the whole file instead.");
}

bool GDriveFileSystem::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	throw NotImplementedException("gdrive: cannot trim in place -- Google Drive has no partial-content API.");
}

} // namespace gdrive
} // namespace duckdb
