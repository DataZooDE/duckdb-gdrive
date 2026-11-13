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
// the session. A final chunk whose every answer was lost is looked up by the
// create's reserved id: published (adopt it) or not (nothing was written).
//
// Every create carries a reserved id (files.generateIds), so every write here
// is safe to retry -- see IsRetrySafe in gdrive_upload_plan.hpp.
//
// files.create when there was no existing file id, files.update (overwrite)
// when OpenFile found one (S-3.9), never a second file sharing the name (which
// would manufacture an R-4 collision).
#include "gdrive_internal.hpp"

namespace duckdb {
namespace gdrive {

namespace {

//! The destination folder of `path`: everything before its last segment, or
//! empty when the path has no folder part beyond the scheme.
std::string ParentPathOf(const std::string &path) {
	auto slash = path.find_last_of('/');
	if (slash == std::string::npos || slash < std::string(GDRIVE_SCHEME).size()) {
		return std::string();
	}
	return path.substr(0, slash);
}

//! Throw for a failed upload call. A 404 that names the PARENT folder id --
//! which is what Drive answers when the destination folder is deleted while
//! a streaming upload is in flight -- is reported as the folder going away,
//! not as "no such file: <target>", which names the wrong thing. Matching on
//! the id rather than on any 404 keeps an expired upload session (also a 404)
//! reported as what it is. Same exception class as ThrowGDriveError.
[[noreturn]] void ThrowUploadError(const GDriveError &error, const GDriveFileHandleImpl &handle) {
	if (error.kind == GDriveErrorKind::NOT_FOUND && !handle.write_parent_id.empty() &&
	    error.message.find(handle.write_parent_id) != std::string::npos) {
		const std::string folder = ParentPathOf(handle.path);
		const std::string folder_desc = folder.empty() ? std::string() : "'" + folder + "' ";
		throw IOException("gdrive: cannot write '%s': the destination folder %sno longer exists (folder id %s; "
		                  "it was deleted or moved during the upload). Nothing was written: the file was "
		                  "never published.",
		                  handle.path, folder_desc, handle.write_parent_id);
	}
	ThrowGDriveError(error, handle.path);
}

//! The handle's reserved id for a CREATE, reserving one now if OpenFile did
//! not. Empty for an overwrite.
const std::string &CreateIdFor(GDriveClient &client, GDriveFileHandleImpl &handle) {
	if (handle.meta.id.empty() && handle.reserved_id.empty()) {
		handle.reserved_id = ReserveFileIds(client, 1, handle.path)[0];
	}
	return handle.reserved_id;
}

} // namespace

std::vector<std::string> ReserveFileIds(GDriveClient &client, int count, const std::string &path) {
	// generateIds is a GET and was already retried with backoff.
	auto resp = client.GenerateFileIds(count);
	std::vector<std::string> ids;
	if (resp.ok && ParseGeneratedFileIds(resp.body, static_cast<std::size_t>(count), ids)) {
		return ids;
	}
	if (!resp.ok && !resp.error.IsRetryable() && resp.error.kind != GDriveErrorKind::UNKNOWN) {
		// Auth, scope, permission: the create would fail the same way, and the
		// user needs that cause (and its exception class), not ours.
		ThrowGDriveError(resp.error, path);
	}
	const std::string why = resp.ok ? std::string("Drive's files.generateIds reply carried no usable ids")
	                                : FormatUserMessage(resp.error, "files.generateIds");
	throw IOException("gdrive: cannot create '%s': could not reserve a file id (%s). Every create carries a "
	                  "reserved id so that a retry cannot make a second file, and it is refused without one. "
	                  "Nothing was written; it is safe to re-run.",
	                  path, why);
}

void SendUploadChunk(GDriveFileHandleImpl &handle) {
	auto client = CreateGDriveClient(handle.auth_context);
	if (handle.upload_session.empty()) {
		// Initiation creates no file (Drive publishes on the final chunk), so
		// a repeated initiation only orphans a session. A create's session
		// still carries a reserved id: that is what lets an ambiguous FINAL
		// chunk be resolved by looking the id up.
		const std::string reserved = CreateIdFor(*client, handle);
		auto start = client->StartResumableUpload(handle.meta.id, handle.write_parent_id, handle.write_name,
		                                          "application/octet-stream", reserved);
		if (!start.ok) {
			ThrowUploadError(start.error, handle);
		}
		handle.upload_session = start.location;
	}
	auto resp = client->UploadChunk(handle.upload_session, *handle.upload, /*final=*/false);
	if (!resp.ok) {
		ThrowUploadError(resp.error, handle);
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
		const auto outcome =
		    resp.ok || handle.reserved_id.empty()
		        ? CreateOutcome::DONE
		        : InterpretCreateReply(resp.http_status != 0, resp.http_status, resp.error.reason, true,
		                               resp.had_ambiguous_attempt);
		if (outcome == CreateOutcome::ID_IN_USE) {
			resp.error.kind = GDriveErrorKind::UNKNOWN;
			resp.error.message = "Drive reports the id reserved for this file (" + handle.reserved_id +
			                     ") is already in use, but no earlier attempt went unanswered, so it is not ours. "
			                     "That is a bug or a concurrent writer; this write published nothing.";
		}
		if (outcome == CreateOutcome::CHECK_RESERVED || outcome == CreateOutcome::ADOPT_RESERVED) {
			// Every answer to the final chunk was lost. The session publishes
			// under the reserved id, so the id says what happened.
			auto meta_resp = client.GetMetadata(handle.reserved_id);
			DriveFileMeta published;
			if (meta_resp.ok && ParseFileMeta(meta_resp.body, published) && !published.trashed &&
			    published.name == handle.write_name) {
				resp = meta_resp;
			} else if (!meta_resp.ok && meta_resp.error.kind == GDriveErrorKind::NOT_FOUND) {
				resp.error.message = "the final chunk of a resumable upload got no usable answer from Drive on "
				                     "any attempt, and nothing is published under the file's reserved id. "
				                     "Nothing was written; it is safe to re-run.";
			}
		}
		if (!resp.ok) {
			ThrowUploadError(resp.error, handle);
		}
		handle.upload_session.clear(); // consumed: nothing left to cancel
	} else {
		// Never outgrew one chunk: one multipart request, as before.
		//
		// A CREATE (no existing id) carries a reserved id, which is what makes
		// it safe to retry after an ambiguous transport failure. Without it, a
		// retry can produce a SECOND file with the same name in the same
		// folder -- and duplicate names are a hard error by design (R-4), so
		// the path becomes unaddressable and the user sees "ambiguous" for a
		// file they wrote once. Costs one extra round trip per created file
		// (none when OpenFile already reserved it with the parent folders).
		// If the reservation fails the create is REFUSED (ReserveFileIds): an
		// unreserved create cannot be retried, and used to be sent anyway.
		const std::string reserved_id = CreateIdFor(client, handle);
		resp = client.Upload(handle.meta.id, handle.write_parent_id, handle.write_name, "application/octet-stream",
		                     buf.Pending(), reserved_id);
		if (!resp.ok) {
			ThrowUploadError(resp.error, handle);
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
