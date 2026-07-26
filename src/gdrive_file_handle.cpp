// S-2.12 / S-3.5..S-3.9 -- per-handle state and the write-on-close lifecycle.
//
// GDriveFileHandle itself is deliberately thin (see gdrive_filesystem.hpp):
// metadata captured once at OpenFile, a sequential cursor, and the write/
// export buffers. The auth context a handle needs for Close() to actually
// talk to Drive lives on GDriveFileHandleImpl (gdrive_internal.hpp), a
// subclass private to this track -- the frozen header has no room for it.
#include "gdrive_internal.hpp"

namespace duckdb {
namespace gdrive {

GDriveFileHandle::GDriveFileHandle(FileSystem &fs, string path, FileOpenFlags flags, DriveFileMeta meta_p)
    : FileHandle(fs, std::move(path), flags), meta(std::move(meta_p)) {
}

GDriveFileHandle::~GDriveFileHandle() {
	// Best-effort: FileHandle's contract does not guarantee Close() was
	// called before destruction on every code path (e.g. an exception
	// unwinding through a scan). We do NOT upload here -- doing I/O from a
	// destructor during unwind is its own hazard, and Close() is the
	// documented place the buffered write goes out. A handle destroyed
	// without Close() simply loses its buffered write, which is the safe
	// failure (no partial file in Drive), matching write_failed semantics.
}

void GDriveFileHandle::Close() {
	if (!is_write || write_failed) {
		// Read handles, and writes that already failed, have nothing to do.
		// Critically: write_failed means Close() must NOT upload whatever
		// was buffered when the failure happened (R-3) -- publishing a
		// truncated file to the user's Drive is worse than failing loudly.
		return;
	}

	// OpenFile only ever constructs GDriveFileHandleImpl (see
	// gdrive_filesystem.cpp), so this downcast is safe for every real handle
	// that reaches here.
	auto &impl = this->Cast<GDriveFileHandleImpl>();
	auto client = CreateGDriveClient(impl.auth_context);
	UploadHandleContents(*client, impl);
}

} // namespace gdrive
} // namespace duckdb
