// S-3.10..S-3.13 -- deletion and directory mutation (D-6, HLD section 8).
//
// Free functions rather than GDriveFileSystem methods (the header is frozen
// and does not declare them); GDriveFileSystem's overrides in
// gdrive_filesystem.cpp resolve the ClientContext/auth/client once and call
// straight through to these.
#include "gdrive_internal.hpp"

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
std::string ResolveParentId(GDriveClient &client, GDrivePathCache &cache, const GDriveAuthContext &auth,
                            const std::string &parent_path) {
	if (parent_path.empty()) {
		return RootFolderFor(auth);
	}
	auto parsed = ParseGDriveUri(std::string(GDRIVE_SCHEME) + parent_path);
	if (!parsed.ok) {
		throw IOException("gdrive: %s", parsed.error);
	}
	auto meta = ResolvePath(cache, client, auth, parsed.uri);
	return meta.id;
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
                    const GDriveUri &source_uri, const GDriveUri &target_uri) {
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

	auto resp = client.Move(source_meta.id, new_parent_id, new_name, old_parent_id);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, source_uri.ToString());
	}

	if (source_uri.kind == GDriveUriKind::PATH) {
		CacheKey src_key {auth.secret_name, auth.drive_id, auth.root_folder_id, CanonicalPathOf(source_uri)};
		cache.InvalidatePrefix(src_key);
	}
	CacheKey dst_key {auth.secret_name, auth.drive_id, auth.root_folder_id, target_uri.ParentPath()};
	cache.InvalidatePrefix(dst_key);
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
