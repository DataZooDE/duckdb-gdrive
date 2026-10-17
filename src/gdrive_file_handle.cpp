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
	// documented place the write is published. A handle destroyed without
	// Close() publishes nothing, matching write_failed semantics; an open
	// resumable session is cancelled by ~GDriveFileHandleImpl.
}

GDriveFileHandleImpl::~GDriveFileHandleImpl() {
	// The one piece of I/O a destructor does: cancel a resumable session a
	// failed or abandoned write left open. Never throws (AbortUpload).
	AbortUpload(*this);
}

void GDriveFileHandle::CancelWrite() noexcept {
	if (!is_write) {
		return;
	}
	// Same outcome as a failed Close(): nothing is published, ever. Mark the
	// handle so a Close() that still follows cannot publish what was written.
	is_write = false;
	write_failed = true;
	auto &impl = this->Cast<GDriveFileHandleImpl>();
	AbortUpload(impl);
	impl.upload.reset();
}

void GDriveFileHandle::Close() {
	if (!is_write) {
		// Read handles have nothing to do.
		return;
	}
	// OpenFile only ever constructs GDriveFileHandleImpl (see
	// gdrive_filesystem.cpp), so this downcast is safe for every real handle
	// that reaches here.
	auto &impl = this->Cast<GDriveFileHandleImpl>();
	is_write = false; // a second Close() must not publish twice
	if (write_failed) {
		// write_failed means Close() must NOT publish whatever was written
		// before the failure (R-3) -- a truncated file in the user's Drive is
		// worse than failing loudly. Cancel the session instead.
		AbortUpload(impl);
		return;
	}
	try {
		auto client = CreateGDriveClient(impl.auth_context);
		UploadHandleContents(*client, impl);
	} catch (...) {
		// A failed close is a failed write: cancel and let the error surface --
		// never report it as success. Nothing is published, unless the final
		// chunk's outcome was ambiguous, which the error then says.
		write_failed = true;
		AbortUpload(impl);
		throw;
	}
	impl.upload.reset();
}

} // namespace gdrive
} // namespace duckdb
