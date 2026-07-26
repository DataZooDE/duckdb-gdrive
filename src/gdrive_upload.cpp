// S-3.5..S-3.9 -- the buffered-upload lifecycle (HLD section 7, R-3).
//
// Sequential writes only: bytes accumulate in GDriveFileHandle::write_buffer
// and nothing reaches Drive until Close(). Close() (gdrive_file_handle.cpp)
// checks write_failed and refuses to call this function at all when a
// previous Write() failed -- publishing a truncated file to the user's Drive
// is worse than failing loudly. This function is the "one resumable upload"
// itself: files.create when there was no existing file id, files.update
// (overwrite) when OpenFile found one (S-3.9), never a second file sharing
// the name (which would manufacture an R-4 collision).
#include "gdrive_internal.hpp"

namespace duckdb {
namespace gdrive {

void UploadHandleContents(GDriveClient &client, GDriveFileHandleImpl &handle) {
	if (handle.write_failed) {
		// Defensive: GDriveFileHandle::Close() already guards this, but this
		// function is reachable on its own, and the rule is absolute (R-3).
		return;
	}

	auto resp = client.Upload(handle.meta.id, handle.write_parent_id, handle.write_name, "application/octet-stream",
	                          handle.write_buffer);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, handle.path);
	}

	DriveFileMeta uploaded;
	if (ParseFileMeta(resp.body, uploaded)) {
		handle.meta = uploaded;
	}
	// No path-cache invalidation here -- see the header comment on this
	// function in gdrive_internal.hpp for why that is safe rather than a
	// latent staleness bug.
}

} // namespace gdrive
} // namespace duckdb
