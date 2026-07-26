#pragma once

// ---------------------------------------------------------------------------
// Internal glue shared ONLY between this track's own translation units
// (gdrive_filesystem.cpp, gdrive_path_cache.cpp, gdrive_file_handle.cpp,
// gdrive_export.cpp, gdrive_upload.cpp, gdrive_mutate.cpp).
//
// This header is NOT part of the frozen contract (docs/implementation-plan.md
// section 3.2) -- it did not exist before fan-out, so nobody else depends on
// it and it is safe to shape freely. It exists because gdrive_filesystem.hpp
// is frozen and cannot grow new fields; the state a handle needs beyond what
// that header declares (an auth context, so Read/Write/Close do not need an
// opener) lives in a subclass declared here instead.
// ---------------------------------------------------------------------------

#include "gdrive_auth.hpp"
#include "gdrive_client.hpp"
#include "gdrive_errors.hpp"
#include "gdrive_filesystem.hpp"
#include "gdrive_stats.hpp"
#include "gdrive_uri.hpp"

#include "duckdb/common/file_opener.hpp"

#include <memory>
#include <string>

namespace duckdb {
namespace gdrive {

//! Convenience overload over gdrive_stats.hpp's CreateGDriveClient(token,
//! drive_id): builds a client straight from an already-resolved auth
//! context, which is what every call site in this track actually has on
//! hand. T2.B (src/gdrive_client.cpp) owns the real factory and the
//! per-process stats registry it feeds.
inline std::unique_ptr<GDriveClient> CreateGDriveClient(const GDriveAuthContext &auth_context) {
	return CreateGDriveClient(auth_context.access_token, auth_context.drive_id);
}

//! Escape a literal for Drive's `q` query syntax: backslash and single-quote
//! are backslash-escaped. This is Drive query syntax, not SQL, and not URL
//! percent-encoding -- a distinct escaping pass from anything the URI parser
//! or the HTTP layer does.
std::string EscapeDriveQueryLiteral(const std::string &value);

//! Translate a classified Drive error into the DuckDB exception HLD section 9
//! specifies and throw it. `context` names the gdrive:// path the user wrote
//! (or the operation being attempted), never a bare file id and never token
//! material (REQ-NF-03).
[[noreturn]] void ThrowGDriveError(const GDriveError &error, const std::string &context);

//! Build the canonical gdrive:// path for a plain (non-ambiguous) child of
//! `parent_path`. `parent_path` has no leading "gdrive://" and no trailing
//! slash; empty means the root.
std::string JoinGDrivePath(const std::string &parent_path, const std::string &name);

//! Read an extension setting (gdrive_permanent_delete, gdrive_docs_export_mime)
//! through the FileOpener, falling back to `default_value` when unset or when
//! there is no opener at all (e.g. a handle-less internal call).
bool GetBoolSetting(optional_ptr<FileOpener> opener, const std::string &name, bool default_value);
std::string GetStringSetting(optional_ptr<FileOpener> opener, const std::string &name,
                              const std::string &default_value);

//! Get a ClientContext out of an opener, or throw an actionable IOException
//! naming `context` when there is none -- every gdrive:// operation needs one
//! to resolve a secret.
ClientContext &RequireClientContext(optional_ptr<FileOpener> opener, const std::string &context);

// ---------------------------------------------------------------------------
// GDriveFileHandle carries no ClientContext/auth state of its own (the header
// is frozen and Read/Write/Close take no opener), so this subclass adds the
// one thing OpenFile learns that Close()/Read() later need: the auth context
// resolved for this path. FileHandle::Cast<GDriveFileHandleImpl> recovers it;
// this is the same pattern core's HTTPFileHandle/HTTPFileSystem pair uses.
// ---------------------------------------------------------------------------
class GDriveFileHandleImpl : public GDriveFileHandle {
public:
	GDriveFileHandleImpl(FileSystem &fs, string path, FileOpenFlags flags, DriveFileMeta meta,
	                      GDriveAuthContext auth_context_p)
	    : GDriveFileHandle(fs, std::move(path), flags, std::move(meta)), auth_context(std::move(auth_context_p)) {
	}

	GDriveAuthContext auth_context;
};

//! The bare, scheme-free, segment-joined path a GDriveUri denotes -- the same
//! string ResolvePath uses to build CacheKey::canonical_path. Meaningless for
//! FILE_ID (returns the empty string); callers needing to invalidate a
//! FILE_ID-addressed subtree cannot key it by path at all, since it was never
//! cached by path in the first place.
std::string CanonicalPathOf(const GDriveUri &uri);

//! Resolve a gdrive:// URI to metadata using the cache (see GDrivePathCache).
//! `client` must already be bound to `auth` (see CreateGDriveClient) -- this
//! function is pure orchestration over the client/cache, with no notion of
//! ClientContext or secrets of its own, so it is shared by GDriveFileSystem's
//! overrides and by the mutate/export/upload helpers alike.
//!
//! More than one match is an ERROR naming both file ids and suggesting the
//! id: form (R-4).
DriveFileMeta ResolvePath(GDrivePathCache &cache, GDriveClient &client, const GDriveAuthContext &auth,
                          const GDriveUri &uri);

//! Same resolution as ResolvePath, but returns false instead of throwing for
//! the "no such file or directory" outcome specifically. Ambiguity (R-4),
//! auth failures and other API errors still throw -- callers with "not
//! found is an empty result, not an error" semantics (Glob, FileExists,
//! DirectoryExists) must use this rather than a broad catch(...), which
//! would silently turn an ambiguity or auth error into "no match" (exactly
//! the failure mode REQ-F-08/R-2 exist to prevent, one layer up).
bool TryResolvePath(GDrivePathCache &cache, GDriveClient &client, const GDriveAuthContext &auth, const GDriveUri &uri,
                    DriveFileMeta &out);

// ---------------------------------------------------------------------------
// Mutation helpers (T3.C, gdrive_mutate.cpp). Kept as free functions rather
// than GDriveFileSystem methods because gdrive_filesystem.hpp is frozen and
// does not declare them; GDriveFileSystem's overrides call straight through,
// resolving the ClientContext/auth/client once and passing them down.
// ---------------------------------------------------------------------------
void MutateRemoveFile(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                      const GDriveUri &uri, bool permanent);
void MutateMoveFile(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                    const GDriveUri &source_uri, const GDriveUri &target_uri);
void MutateCreateDirectory(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                           const GDriveUri &uri);
void MutateRemoveDirectory(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                           const GDriveUri &uri, bool permanent);

// ---------------------------------------------------------------------------
// Export helpers (T3.A, gdrive_export.cpp). Native Google formats (Sheets,
// Docs, Slides, ...) have no byte stream; alt=media 403s with
// NOT_DOWNLOADABLE, so the caller must switch to files.export instead.
// ---------------------------------------------------------------------------

//! The target MIME type to export `meta` as, honouring the
//! gdrive_docs_export_mime setting (D-7). Sheets always export to text/csv,
//! regardless of the setting.
std::string ExportMimeTypeFor(const DriveFileMeta &meta, const std::string &docs_export_mime_setting);

//! Fetch the whole export once (Export does not support Range) and return the
//! bytes. Throws via ThrowGDriveError on failure.
std::string FetchExport(GDriveClient &client, const DriveFileMeta &meta, const std::string &export_mime_type,
                        const std::string &context_path);

// ---------------------------------------------------------------------------
// Upload helper (T3.B, gdrive_upload.cpp). One resumable upload on Close();
// never called when write_failed is set (a partial write must never publish
// a truncated file to Drive).
//
// Deliberately takes no GDrivePathCache: GDriveFileHandle::Close() has no
// route to the owning GDriveFileSystem's private `cache` member (the header
// is frozen and declares no friendship), so this path cannot invalidate the
// resolver cache directly. That is safe rather than a latent bug: OpenFile
// always re-fetches a PATH-resolved leaf's own metadata fresh (see
// gdrive_filesystem.cpp), which covers overwrite staleness, and a brand-new
// file name has no prior cache entry to go stale. RemoveFile/MoveFile/
// CreateDirectory/RemoveDirectory DO invalidate directly, because they run
// as GDriveFileSystem methods with real access to `cache`.
// ---------------------------------------------------------------------------
void UploadHandleContents(GDriveClient &client, GDriveFileHandleImpl &handle);

} // namespace gdrive
} // namespace duckdb
