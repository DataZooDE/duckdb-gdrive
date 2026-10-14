// S-3.5..S-3.9 -- the write-path upload lifecycle (HLD section 7, R-3), now
// STREAMING: a write handle holds at most one chunk in memory.
//
// Sequential writes only. GDriveFileSystem::Write fills the handle's one-chunk
// buffer; when it overflows, SendUploadChunk opens a Drive resumable upload
// session (lazily, once) and sends the full chunk. Close() then calls
// UploadHandleContents, which either sends the whole small file as ONE
// multipart request (it never outgrew the chunk -- the old, cheap path), or
// sends the final chunk carrying the total, which is the moment Drive
// publishes the file. Until then nothing is visible in the folder, and an
// overwritten file keeps its old content (verified live 2026-10-14), so a
// failure anywhere mid-stream leaves nothing behind once AbortUpload cancels
// the session -- except a final chunk whose every answer was lost, which
// fails with the "may or may not have been applied" error instead.
//
// files.create when there was no existing file id, files.update (overwrite)
// when OpenFile found one (S-3.9), never a second file sharing the name (which
// would manufacture an R-4 collision).
#include "gdrive_internal.hpp"

namespace duckdb {
namespace gdrive {

void SendUploadChunk(GDriveFileHandleImpl &handle) {
	auto client = CreateGDriveClient(handle.auth_context);
	if (handle.upload_session.empty()) {
		// Initiation creates no file (Drive publishes on the final chunk), so
		// it needs no reserved id to be retry-safe -- unlike a multipart
		// create, a repeated initiation only orphans a session.
		auto start = client->StartResumableUpload(handle.meta.id, handle.write_parent_id, handle.write_name,
		                                          "application/octet-stream");
		if (!start.ok) {
			ThrowGDriveError(start.error, handle.path);
		}
		handle.upload_session = start.location;
	}
	auto resp = client->UploadChunk(handle.upload_session, *handle.upload, /*final=*/false);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, handle.path);
	}
	handle.upload->MarkChunkSent();
}

void AbortUpload(GDriveFileHandleImpl &handle) noexcept {
	if (handle.upload_session.empty()) {
		return;
	}
	const std::string session = std::move(handle.upload_session);
	handle.upload_session.clear();
	try {
		CreateGDriveClient(handle.auth_context)->CancelUpload(session);
	} catch (...) {
		// Best effort: an abandoned session expires on its own, and it never
		// published anything.
	}
}

void UploadHandleContents(GDriveClient &client, GDriveFileHandleImpl &handle) {
	if (handle.write_failed || !handle.upload) {
		// Defensive: GDriveFileHandle::Close() already guards this, but this
		// function is reachable on its own, and the rule is absolute (R-3).
		return;
	}
	auto &buf = *handle.upload;

	DriveResponse resp;
	if (buf.OnClose() == ChunkedUploadBuffer::CloseAction::FINAL_CHUNK) {
		// The final chunk carries the total; its 200/201 IS the publish, and
		// its body the file's metadata. A lost reply is recovered inside
		// UploadChunk by asking the session, which then answers 200 + file.
		resp = client.UploadChunk(handle.upload_session, buf, /*final=*/true);
		if (!resp.ok) {
			ThrowGDriveError(resp.error, handle.path);
		}
		handle.upload_session.clear(); // consumed: nothing left to cancel
	} else {
		// Never outgrew one chunk: one multipart request, as before.
		//
		// A CREATE (no existing id) reserves an id first, which is what makes
		// it safe to retry after an ambiguous transport failure. Without it, a
		// retry can produce a SECOND file with the same name in the same
		// folder -- and duplicate names are a hard error by design (R-4), so
		// the path becomes unaddressable and the user sees "ambiguous" for a
		// file they wrote once. Costs one extra round trip per created file.
		// If the reservation itself fails we simply proceed without one: a
		// create that cannot be retried is strictly better than no create.
		std::string reserved_id;
		if (handle.meta.id.empty()) {
			auto id_resp = client.GenerateFileId();
			if (id_resp.ok) {
				ParseGeneratedFileId(id_resp.body, reserved_id);
			}
		}
		resp = client.Upload(handle.meta.id, handle.write_parent_id, handle.write_name, "application/octet-stream",
		                     buf.Pending(), reserved_id);
		if (!resp.ok) {
			ThrowGDriveError(resp.error, handle.path);
		}
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
