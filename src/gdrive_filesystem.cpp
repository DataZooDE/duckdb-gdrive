// S-2.2/S-2.6..S-2.19, S-3.10..S-3.13 -- GDriveFileSystem: registration,
// resolution, reads, listing/glob, and the small always-throw surface
// (positional Write, Truncate, Trim). See gdrive_filesystem.hpp for the
// contract and docs/hld.md sections 3, 4, 6, 8 for the design this follows.
#include "gdrive_internal.hpp"
#include "gdrive_glob.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

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
	if (FileOpener::TryGetCurrentSetting(opener, name, value)) {
		return value.GetValue<bool>();
	}
	return default_value;
}

uint64_t GetUBigIntSetting(optional_ptr<FileOpener> opener, const std::string &name, uint64_t default_value) {
	if (!opener) {
		return default_value;
	}
	Value value;
	if (FileOpener::TryGetCurrentSetting(opener, name, value)) {
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
	if (FileOpener::TryGetCurrentSetting(opener, name, value)) {
		return value.ToString();
	}
	return default_value;
}

ClientContext &RequireClientContext(optional_ptr<FileOpener> opener, const std::string &context) {
	auto client_context = FileOpener::TryGetClientContext(opener);
	if (!client_context) {
		throw IOException("gdrive: no active connection context available to resolve '%s'", context);
	}
	return *client_context;
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
	return matches;
}

//! One files.list call for ALL children of `parent_id`, paginated to
//! exhaustion. Used by ListFiles/Glob, which need the whole folder rather
//! than one name.
std::vector<DriveFileMeta> ListChildren(GDriveClient &client, const std::string &parent_id,
                                        const std::string &context_path) {
	std::string query = "'" + parent_id + "' in parents and trashed=false";
	std::vector<DriveFileMeta> children;
	std::string page_token;
	do {
		auto resp = client.List(query, page_token);
		if (!resp.ok) {
			ThrowGDriveError(resp.error, context_path);
		}
		std::vector<DriveFileMeta> page;
		std::string next;
		if (!ParseFileList(resp.body, page, next)) {
			throw IOException("gdrive: malformed listing response for '%s'", context_path);
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
                    DriveFileMeta &out) {
	std::string context_path = uri.ToString();

	if (uri.kind == GDriveUriKind::FILE_ID) {
		// Direct form: zero resolution (files.list) calls (S-2.9). The single
		// files.get below is metadata lookup, not resolution.
		//
		// Served from the metadata cache when it is warm. DuckDB opens a
		// handle PER THREAD for a parallel scan, so without this the id:
		// form -- the one documented as the zero-round-trip fast path --
		// still cost one files.get per thread.
		CacheKey meta_key;
		meta_key.secret_name = auth.secret_name;
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
			                                 return true;
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

		CacheKey key {auth.secret_name, auth.drive_id, auth.root_folder_id, canonical};
		DriveFileMeta cached;
		if (cache.TryGet(key, cached)) {
			meta = cached;
			have_meta = true;
			parent_id = meta.id;
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
                          const GDriveUri &uri) {
	DriveFileMeta meta;
	if (!TryResolvePath(cache, client, auth, uri, meta)) {
		throw IOException("gdrive: no such file or directory: '%s'", uri.ToString());
	}
	return meta;
}

// ---------------------------------------------------------------------------
// GDriveFileSystem
// ---------------------------------------------------------------------------

GDriveFileSystem::GDriveFileSystem() {
}

GDriveFileSystem::~GDriveFileSystem() {
}

bool GDriveFileSystem::CanHandleFile(const string &fpath) {
	return IsGDriveUri(fpath);
}

DriveFileMeta GDriveFileSystem::ResolveOrThrow(ClientContext &context, const GDriveUri &uri) {
	if (!HasAnyGDriveSecret(context)) {
		throw IOException(
		    "gdrive: no gdrive secret configured for '%s' -- run CREATE SECRET (TYPE gdrive, ...) first.",
		    uri.ToString());
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

unique_ptr<FileHandle> GDriveFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                  optional_ptr<FileOpener> opener) {
	auto &context = RequireClientContext(opener, path);
	auto parsed = ParseGDriveUri(path);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	if (!HasAnyGDriveSecret(context)) {
		throw IOException(
		    "gdrive: no gdrive secret configured for '%s' -- run CREATE SECRET (TYPE gdrive, ...) first.", path);
	}
	auto auth = GetAuthContext(context, path);
	auto client = CreateGDriveClient(auth);

	if (flags.OpenForWriting()) {
		// Sequential, buffered write only -- nothing reaches Drive until
		// Close() (HLD section 7, R-3).
		std::string parent_id;
		std::string name;
		std::string existing_id;

		if (parsed.uri.kind == GDriveUriKind::FILE_ID) {
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
			for (size_t i = 0; i + 1 < parsed.uri.segments.size(); i++) {
				const auto &segment = parsed.uri.segments[i];
				parent_path_accum += (parent_path_accum.empty() ? "" : "/") + segment;

				CacheKey key {auth.secret_name, auth.drive_id, auth.root_folder_id, parent_path_accum};
				DriveFileMeta cached;
				if (cache.TryGet(key, cached)) {
					parent_id = cached.id;
					continue;
				}

				auto matches = ListByName(*client, parent_id, segment, /*require_folder=*/true, path);
				if (matches.empty()) {
					auto resp = client->Upload("", parent_id, segment, "application/vnd.google-apps.folder", "");
					if (!resp.ok) {
						ThrowGDriveError(resp.error, path);
					}
					DriveFileMeta created;
					if (!ParseFileMeta(resp.body, created)) {
						throw IOException("gdrive: malformed metadata response creating folder for '%s'", path);
					}
					cache.Put(key, created);
					parent_id = created.id;
				} else if (matches.size() > 1) {
					ThrowAmbiguous(matches, path);
				} else {
					cache.Put(key, matches[0]);
					parent_id = matches[0].id;
				}
			}

			// Overwrite semantics (S-3.9): if a file with this name already
			// exists in the (now-resolved) parent folder, remember its id so
			// Close() updates it via files.update rather than creating a
			// second file with the same name -- which would manufacture an
			// R-4 collision.
			auto existing_matches = ListByName(*client, parent_id, name, /*require_folder=*/false, path);
			if (existing_matches.size() > 1) {
				ThrowAmbiguous(existing_matches, path);
			} else if (existing_matches.size() == 1) {
				existing_id = existing_matches[0].id;
			}
		}

		DriveFileMeta placeholder;
		placeholder.id = existing_id;
		placeholder.name = name;
		auto handle = make_uniq<GDriveFileHandleImpl>(*this, path, flags, placeholder, auth);
		handle->is_write = true;
		handle->write_parent_id = parent_id;
		handle->write_name = name;
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

	auto meta = ResolvePath(cache, *client, auth, parsed.uri);

	// The path-walk cache may serve a leaf entry captured during an earlier
	// Glob/ListFiles call; OpenFile always re-fetches that leaf's own
	// metadata fresh so size and GetVersionTag (headRevisionId) reflect
	// Drive's CURRENT state (S-2.16). The FILE_ID form already did exactly
	// this files.get inside ResolvePath, so skip the duplicate call there.
	CacheKey meta_identity;
	meta_identity.secret_name = auth.secret_name;
	meta_identity.drive_id = auth.drive_id;
	meta_identity.root_folder_id = auth.root_folder_id;

	if (parsed.uri.kind != GDriveUriKind::FILE_ID) {
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
			return true;
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
			key.secret_name = auth.secret_name;
			key.drive_id = auth.drive_id;
			key.root_folder_id = auth.root_folder_id;
			key.canonical_path = CanonicalPathOf(parsed.uri);
			cache.InvalidatePrefix(key);
			meta = ResolvePath(cache, *client, auth, parsed.uri);
		}
	}

	auto handle = make_uniq<GDriveFileHandleImpl>(*this, path, flags, meta, auth);
	handle->block_size = static_cast<idx_t>(GetUBigIntSetting(opener, "gdrive_block_size_bytes", 16ULL * 1024 * 1024));
	blocks.SetCapacity(static_cast<idx_t>(GetUBigIntSetting(opener, "gdrive_block_cache_bytes", 256ULL * 1024 * 1024)));
	handle->block_key = auth.secret_name + '\x1f' + auth.drive_id + '\x1f' + auth.root_folder_id + '\x1f' +
	                    meta.id + '\x1f' + meta.head_revision_id;

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
}

//! A cached file id can go dead under us: another client deletes the file and
//! recreates one at the same path, which Drive gives a NEW id.
//!
//! This used to be handled in OpenFile, by noticing that the metadata refresh
//! 404'd. Caching metadata removed that probe -- and the e2e test for it
//! failed immediately, which is the only reason this is here rather than
//! shipped broken.
//!
//! Handling it at READ time is strictly better than the version it replaces:
//! it also covers a file deleted AFTER the handle was opened, which no amount
//! of validation at open could catch.
//!
//! Returns true if the handle now points at a live, different file id.
bool GDriveFileSystem::TryRecoverStaleHandle(GDriveFileHandle &base) {
	auto &h = base.Cast<GDriveFileHandleImpl>();
	auto parsed = ParseGDriveUri(h.path);
	if (!parsed.ok || parsed.uri.kind == GDriveUriKind::FILE_ID) {
		// An explicit gdrive://id: that 404s is genuinely gone. Re-resolving
		// would mean inventing a different file than the one asked for.
		return false;
	}

	CacheKey key;
	key.secret_name = h.auth_context.secret_name;
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
				    auto r = client_for_block->Download(h.meta.id, static_cast<int64_t>(start),
				                                        static_cast<int64_t>(start + len - 1));
				    if (!r.ok) {
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
		return;
	}

	auto client = CreateGDriveClient(h.auth_context);
	// Inclusive end, as HTTP Range and Drive both define it.
	int64_t end = static_cast<int64_t>(location) + nr_bytes - 1;

	auto resp = client->Download(h.meta.id, static_cast<int64_t>(location), end);
	if (!resp.ok && resp.error.kind == GDriveErrorKind::NOT_FOUND && TryRecoverStaleHandle(h)) {
		// The path still exists, under a new id. Retry ONCE against it: a
		// loop here would spin against a file being rewritten repeatedly.
		client = CreateGDriveClient(h.auth_context);
		resp = client->Download(h.meta.id, static_cast<int64_t>(location), end);
	}
	if (!resp.ok) {
		ThrowGDriveError(resp.error, h.path);
	}

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
			                  h.path, static_cast<unsigned long long>(avail),
			                  static_cast<unsigned long long>(location));
		}
		src += location;
		avail -= static_cast<size_t>(location);
	}

	if (avail < static_cast<size_t>(nr_bytes)) {
		throw IOException("gdrive: short read on '%s': requested %lld bytes at offset %llu, got %llu",
		                  h.path, static_cast<long long>(nr_bytes), static_cast<unsigned long long>(location),
		                  static_cast<unsigned long long>(avail));
	}
	memcpy(buffer, src, static_cast<size_t>(nr_bytes));


	// Deliberately does NOT advance h.position.
	//
	// This overload is pread(2): it takes an explicit offset and must not
	// disturb the shared cursor. DuckDB's parallel Parquet reader issues
	// positional reads for different row groups against ONE handle from
	// several threads at once, so writing h.position here was both a data
	// race and semantically wrong. The sequential overload owns the cursor.
}

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
	    "Write sequentially and close the file; the whole buffered content is uploaded in one request.");
}

int64_t GDriveFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = handle.Cast<GDriveFileHandleImpl>();
	if (!h.is_write) {
		throw IOException("gdrive: handle for '%s' was not opened for writing", h.path);
	}
	if (h.write_failed) {
		throw IOException("gdrive: a previous write to '%s' already failed; this handle can only be closed", h.path);
	}
	try {
		h.write_buffer.append(reinterpret_cast<const char *>(buffer), static_cast<size_t>(nr_bytes));
	} catch (...) {
		// Anything short of a full, contiguous append leaves the buffer in
		// an unknown state; mark the write failed so Close() refuses to
		// upload a possibly-truncated file (R-3).
		h.write_failed = true;
		throw;
	}
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
bool GDriveFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	auto &context = RequireClientContext(opener, filename);
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

bool GDriveFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	auto &context = RequireClientContext(opener, directory);
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

void GDriveFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	auto &context = RequireClientContext(opener, directory);
	auto parsed = ParseGDriveUri(directory);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	auto auth = GetAuthContext(context, directory);
	auto client = CreateGDriveClient(auth);
	MutateCreateDirectory(*client, cache, auth, parsed.uri);
}

void GDriveFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	auto &context = RequireClientContext(opener, directory);
	auto parsed = ParseGDriveUri(directory);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	auto auth = GetAuthContext(context, directory);
	auto client = CreateGDriveClient(auth);
	bool permanent = GetBoolSetting(opener, "gdrive_permanent_delete", false);
	MutateRemoveDirectory(*client, cache, auth, parsed.uri, permanent);
}

bool GDriveFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
                                 FileOpener *opener) {
	optional_ptr<FileOpener> op(opener);
	auto &context = RequireClientContext(op, directory);
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
}

void GDriveFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	auto &context = RequireClientContext(opener, source);
	auto src_parsed = ParseGDriveUri(source);
	if (!src_parsed.ok) {
		throw IOException("gdrive: %s", src_parsed.error);
	}
	auto dst_parsed = ParseGDriveUri(target);
	if (!dst_parsed.ok) {
		throw IOException("gdrive: %s", dst_parsed.error);
	}
	auto auth = GetAuthContext(context, source);
	auto client = CreateGDriveClient(auth);
	bool permanent = GetBoolSetting(opener, "gdrive_permanent_delete", false);
	MutateMoveFile(*client, cache, auth, src_parsed.uri, dst_parsed.uri, permanent);
}

void GDriveFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto &context = RequireClientContext(opener, filename);
	auto parsed = ParseGDriveUri(filename);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	auto auth = GetAuthContext(context, filename);
	auto client = CreateGDriveClient(auth);
	bool permanent = GetBoolSetting(opener, "gdrive_permanent_delete", false);
	MutateRemoveFile(*client, cache, auth, parsed.uri, permanent);
}

vector<OpenFileInfo> GDriveFileSystem::Glob(const string &path, FileOpener *opener) {
	optional_ptr<FileOpener> op(opener);
	auto &context = RequireClientContext(op, path);
	// Deliberately NOT swallowed into "no matches": a missing secret is a
	// configuration error, not an empty result, and reporting it as zero
	// rows is exactly the misleading-failure pattern R-2/REQ-F-08 exist to
	// prevent, one layer up from where those requirements originally target.
	if (!HasAnyGDriveSecret(context)) {
		throw IOException(
		    "gdrive: no gdrive secret configured for '%s' -- run CREATE SECRET (TYPE gdrive, ...) first.", path);
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
	if (parsed.uri.kind == GDriveUriKind::FILE_ID || !HasGlobMetacharacters(path)) {
		vector<OpenFileInfo> result;
		auto auth = GetAuthContext(context, path);
		auto client = CreateGDriveClient(auth);
		DriveFileMeta meta;
		if (TryResolvePath(cache, *client, auth, parsed.uri, meta)) {
			result.emplace_back(path);
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
		std::function<void(const std::string &, const std::string &)> list_folder;
		list_folder = [&](const std::string &folder_id, const std::string &rel_prefix) {
			auto children = ListChildren(*client, folder_id, path);
			for (auto &child : children) {
				std::string rel = rel_prefix.empty() ? child.name : (rel_prefix + "/" + child.name);
				listing.emplace_back(rel, child);
				if (split.recursive && child.IsFolder()) {
					list_folder(child.id, rel);
				}
			}
		};
		list_folder(parent_id, "");

		for (auto &entry : listing) {
			const std::string &rel_path = entry.first;
			const DriveFileMeta &meta = entry.second;
			std::string full_rel_path = split.literal_prefix.empty() ? rel_path : (split.literal_prefix + "/" + rel_path);
			if (!MatchPath(one_pattern, full_rel_path)) {
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
		}
	}
	return result;
}

void GDriveFileSystem::FileSync(FileHandle &handle) {
	// Writes are buffered locally until Close() (HLD section 7); there is
	// nothing Drive-side to flush before that, so this is a deliberate no-op
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
