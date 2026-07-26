// S-3.10..S-3.13 -- deletion and directory mutation (D-6, HLD section 8).
//
// Free functions rather than GDriveFileSystem methods (the header is frozen
// and does not declare them); GDriveFileSystem's overrides in
// gdrive_filesystem.cpp resolve the ClientContext/auth/client once and call
// straight through to these.
#include "gdrive_internal.hpp"

#include <algorithm>
#include <vector>

namespace duckdb {
namespace gdrive {

namespace {

std::string RootFolderFor(const GDriveAuthContext &auth) {
	if (!auth.root_folder_id.empty()) {
		return auth.root_folder_id;
	}
	if (!auth.drive_id.empty()) {
		return auth.drive_id;
	}
	return "root";
}

//! Resolve the immediate parent folder named by a PATH-kind URI's
//! ParentPath(), or the configured root when the path has no parent segment.
//!
//! BUG FIX (live run 2026-07-26): every call site passes `uri.ParentPath()`
//! (or, transitively, this function's own `parent_path` parameter), and
//! GDriveUri::ParentPath() already returns a FULL "gdrive://..." string
//! (gdrive_uri.cpp), not a bare canonical path. Re-prepending GDRIVE_SCHEME
//! here doubled it -- "gdrive://" + "gdrive://scratch/x" -> parsed as the
//! single segment "gdrive:" followed by "scratch"/"x", which then 404'd with
//! a mangled path in the error message. This is why MoveFile (the rename
//! DuckDB's COPY writer performs when overwriting an existing gdrive:// file
//! via a tmp file, see bind_copy.cpp's ResolveUseTmpFile) failed on every
//! overwrite: a brand-new file's write path never calls this function at
//! all (OpenFile walks/creates its own parent chain inline), but overwriting
//! an EXISTING file switches DuckDB to write-tmp-then-MoveFile, and MoveFile
//! resolves both parents through here.
std::string ResolveParentId(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                            const std::string &parent_path) {
	if (parent_path.empty()) {
		return RootFolderFor(auth);
	}
	auto parsed = ParseGDriveUri(parent_path);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	auto meta = ResolvePath(cache, client, auth, parsed.uri);
	return meta.id;
}

//! One files.list call for children of `parent_id` named exactly `name`,
//! paginated to exhaustion. Deliberately NOT filtered by mimeType: R-4
//! ambiguity does not care whether the colliding object is a file or a
//! folder, so neither should this. (Mirrors ListByName in
//! gdrive_filesystem.cpp, which is anonymous-namespace-local there and so
//! not reusable from this translation unit -- duplicated rather than
//! plumbed through gdrive_internal.hpp for the one call site here.)
std::vector<DriveFileMeta> ListFilesNamedInParent(GDriveClient &client, const std::string &parent_id,
                                                  const std::string &name, const std::string &context_path) {
	std::string query =
	    "'" + parent_id + "' in parents and name='" + EscapeDriveQueryLiteral(name) + "' and trashed=false";
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

} // namespace

void MutateRemoveFile(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                      const GDriveUri &uri, bool permanent) {
	auto meta = ResolvePath(cache, client, auth, uri);
	if (meta.IsFolder()) {
		throw IOException("gdrive: '%s' is a directory -- use RemoveDirectory, not RemoveFile", uri.ToString());
	}

	auto resp = client.Delete(meta.id, permanent);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, uri.ToString());
	}

	if (uri.kind == GDriveUriKind::PATH) {
		CacheKey key {auth.secret_name, auth.drive_id, auth.root_folder_id, CanonicalPathOf(uri)};
		cache.InvalidatePrefix(key);
	}
}

void MutateMoveFile(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                    const GDriveUri &source_uri, const GDriveUri &target_uri, bool permanent_delete_replaced) {
	// NOTE (HLD section 3/7): this is NOT atomic. DuckDB's storage manager
	// documents an atomic-rename contract for MoveFile; files.update on
	// parents/name is a read-then-write against Drive with no compare-and-
	// swap, so a concurrent reader could observe the file at neither the old
	// nor the new location for a brief window, or (in the rare case Delete
	// races a Move) find it gone entirely. Documented, not papered over.
	if (target_uri.kind == GDriveUriKind::FILE_ID) {
		throw InvalidInputException(
		    "gdrive: MoveFile target must be a gdrive://path naming the new location, not an id: form");
	}

	auto source_meta = ResolvePath(cache, client, auth, source_uri);

	std::string old_parent_id;
	if (source_uri.kind == GDriveUriKind::PATH) {
		old_parent_id = ResolveParentId(client, cache, auth, source_uri.ParentPath());
	}
	// For a FILE_ID-addressed source we have no path to derive the current
	// parent from; GDriveClient::Move is called with an empty old_parent_id
	// in that case; see gdrive_client.hpp's Move() -- best-effort until that
	// track confirms whether an empty old_parent_id is accepted as "add the
	// new parent without removing an old one".

	std::string new_name = target_uri.FileName();
	if (new_name.empty()) {
		throw IOException("gdrive: move target '%s' does not name a file", target_uri.ToString());
	}
	std::string new_parent_id = ResolveParentId(client, cache, auth, target_uri.ParentPath());

	// BUG FIX (S-3.9, live run 2026-07-26): DuckDB's COPY writer overwrites
	// an existing gdrive:// target by writing to a tmp file and then calling
	// MoveFile onto the final name once it discovers the target already
	// exists (bind_copy.cpp's ResolveUseTmpFile). This function used to just
	// call client.Move() and never looked at what was already sitting at the
	// destination -- Drive happily allows two objects with the same name in
	// the same parent, so "overwrite" silently produced two files sharing
	// one name, which is exactly the R-4 ambiguity the resolver refuses to
	// guess at on the next read.
	//
	// Reconcile the destination name BEFORE moving. Exclude the source
	// file's own id from the count first: if source and target happen to
	// resolve to the same object (e.g. a no-op rename), that entry IS the
	// file being moved, not a distinct collision.
	auto existing_at_dest = ListFilesNamedInParent(client, new_parent_id, new_name, target_uri.ToString());
	existing_at_dest.erase(std::remove_if(existing_at_dest.begin(), existing_at_dest.end(),
	                                      [&](const DriveFileMeta &m) { return m.id == source_meta.id; }),
	                       existing_at_dest.end());
	if (existing_at_dest.size() > 1) {
		// 2+ pre-existing files already sharing the destination name can
		// only be leftover from an earlier buggy run (this bug, before the
		// fix below) or a concurrent writer. Same R-4 posture as
		// ResolvePath's ThrowAmbiguous: never guess which one the caller
		// means to replace -- name every id so they can clean up by hand
		// with the gdrive://id:<fileId> form.
		std::string ids;
		for (size_t i = 0; i < existing_at_dest.size(); i++) {
			if (i > 0) {
				ids += ", ";
			}
			ids += existing_at_dest[i].id;
		}
		throw IOException(
		    "gdrive: cannot overwrite '%s': %d files already share that name in the destination folder "
		    "(ids: %s). Remove or rename them with the gdrive://id:<fileId> form before overwriting.",
		    target_uri.ToString(), static_cast<int>(existing_at_dest.size()), ids);
	}

	auto resp = client.Move(source_meta.id, new_parent_id, new_name, old_parent_id);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, source_uri.ToString());
	}

	// Ordering (deliberate, and the reason this runs AFTER the move rather
	// than before it): for the interval between the two calls there can be
	// two files sharing the destination name, which reads as ambiguous to a
	// concurrent reader (R-4) -- annoying, but recoverable by hand and never
	// silently wrong, since ResolvePath refuses to guess and just errors.
	// Deleting the stale file FIRST and moving SECOND would fail the other
	// way: if the move then failed (network blip, permission change,
	// Drive being Drive), the old content is already gone and the new
	// content never arrived -- real, silent data loss, which is strictly
	// worse than a duplicate. So: never destroy data that has not already
	// been replaced.
	//
	// Trash vs. permanent delete for the replaced file: D-6 makes an
	// explicit RemoveFile default to trash, because trash is recoverable.
	// An overwrite is not quite the same situation -- a workload that
	// repeatedly overwrites the same report path (a realistic pattern per
	// the BRD) would otherwise silently accumulate one trashed copy per
	// run forever, with no obvious reason for the user to ever go empty
	// the trash. Rather than invent separate overwrite-specific default
	// behaviour, this honours the same `gdrive_permanent_delete` setting
	// RemoveFile already reads (still defaulting to trash/recoverable):
	// a user who wants replaced files gone for good has already told the
	// extension so at the connection level, and an overwrite should keep
	// behaving the way an explicit RemoveFile of the old file would.
	if (existing_at_dest.size() == 1) {
		auto del_resp = client.Delete(existing_at_dest[0].id, permanent_delete_replaced);
		if (!del_resp.ok) {
			ThrowGDriveError(del_resp.error, target_uri.ToString());
		}
	}

	if (source_uri.kind == GDriveUriKind::PATH) {
		CacheKey src_key {auth.secret_name, auth.drive_id, auth.root_folder_id, CanonicalPathOf(source_uri)};
		cache.InvalidatePrefix(src_key);
	}
	// BUG FIX (live run 2026-07-26): this used to build the key from
	// `target_uri.ParentPath()`, which -- like ResolveParentId's bug above
	// -- is a FULL "gdrive://..." string, not the bare canonical path every
	// other CacheKey in this file uses (CanonicalPathOf()). CacheKey's
	// identity_prefix + canonical_path never matched any real cache entry
	// (they are all keyed by bare paths), so this invalidation was a no-op:
	// overwriting an existing gdrive:// file via COPY (which routes through
	// write-tmp-then-MoveFile once the target exists, per DuckDB's
	// ResolveUseTmpFile) moved the new content into place on Drive
	// correctly, but a stale cached leaf entry for the destination's OLD
	// file id/name kept being served back to readers, so the read-back
	// after an overwrite silently returned the previous file's content.
	// Invalidating the destination's own canonical path (exact leaf, plus
	// any descendants) is what actually needs to go.
	if (target_uri.kind == GDriveUriKind::PATH) {
		CacheKey dst_key {auth.secret_name, auth.drive_id, auth.root_folder_id, CanonicalPathOf(target_uri)};
		cache.InvalidatePrefix(dst_key);
	}
}

void MutateCreateDirectory(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                           const GDriveUri &uri) {
	if (uri.kind != GDriveUriKind::PATH || uri.segments.empty()) {
		throw InvalidInputException("gdrive: CreateDirectory needs a gdrive://path naming a folder, not '%s'",
		                            uri.ToString());
	}

	std::string parent_path = uri.ParentPath();
	std::string name = uri.FileName();
	std::string parent_id = ResolveParentId(client, cache, auth, parent_path);

	// Drive folders are files with a folder mimeType (HLD section 3). The
	// frozen GDriveClient interface exposes only Upload() for files.create --
	// there is no dedicated "create folder" call -- so an empty-content
	// upload with the folder mimeType is the closest available primitive.
	auto resp = client.Upload("", parent_id, name, "application/vnd.google-apps.folder", "");
	if (!resp.ok) {
		ThrowGDriveError(resp.error, uri.ToString());
	}

	CacheKey key {auth.secret_name, auth.drive_id, auth.root_folder_id, parent_path};
	cache.InvalidatePrefix(key);
}

void MutateRemoveDirectory(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                           const GDriveUri &uri, bool permanent) {
	auto meta = ResolvePath(cache, client, auth, uri);
	if (!meta.IsFolder()) {
		throw IOException("gdrive: '%s' is not a directory", uri.ToString());
	}

	auto resp = client.Delete(meta.id, permanent);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, uri.ToString());
	}

	if (uri.kind == GDriveUriKind::PATH) {
		CacheKey key {auth.secret_name, auth.drive_id, auth.root_folder_id, CanonicalPathOf(uri)};
		cache.InvalidatePrefix(key);
	}
}

} // namespace gdrive
} // namespace duckdb
