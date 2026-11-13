#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Pure helpers for STREAMING writes over a Drive resumable upload session.
// No duckdb.hpp, no I/O, so the Catch2 binary exercises them directly; the
// PUT loop that drives them is GDriveClientImpl::UploadChunk.
//
// Drive's resumable contract (verified live 2026-10-14):
//   * every chunk except the last MUST be a multiple of 256 KiB -- a smaller
//     non-final chunk is a 400 ("required to be equal or greater than 262144");
//   * a non-final chunk may say "Content-Range: bytes a-b/*" (total unknown);
//     Drive answers 308 with "Range: bytes=0-N" (what it holds so far);
//   * the final chunk carries the total and answers 200/201 with the file --
//     NOTHING is visible in the folder before that, and an overwritten file
//     keeps its old content until then;
//   * DELETE on the session URI cancels it (Drive answers 499).
// ---------------------------------------------------------------------------

//! 256 KiB -- the granularity Drive requires for every non-final chunk.
constexpr std::uint64_t kDriveUploadChunkMultiple = 262144;

//! Upper bound on a chunk: the chunk IS a write handle's memory footprint, so
//! one SET must not be able to make it unbounded. 1 GiB.
constexpr std::uint64_t kDriveUploadChunkMax = 4096 * kDriveUploadChunkMultiple;

//! Round `requested` DOWN to a positive multiple of 256 KiB, at most
//! kDriveUploadChunkMax. Never returns 0.
std::uint64_t NormalizeDriveChunkSize(std::uint64_t requested);

//! Parse the "Range" header of a 308 Resume Incomplete into the next byte
//! Drive expects: "bytes=0-262143" -> 262144. An ABSENT header (empty string)
//! means Drive holds nothing yet -> 0. False for anything else.
bool ParseDriveRangeNext(const std::string &range_header, std::uint64_t &next);

//! A resumable session URI (the Location of the initiation response) is a
//! credential for the upload, so it is only ever followed over https to the
//! Drive host. On success `path_and_query` is the part after the origin.
bool DriveSessionPath(const std::string &session_uri, std::string &path_and_query);

//! The streaming-write state machine behind a write handle: holds AT MOST one
//! chunk of not-yet-sent bytes, so a write of any size needs ~one chunk of
//! memory. The caller (GDriveFileSystem::Write) loops:
//!
//!   while (bytes left) { if (Full()) { send Pending() as a NON-final chunk;
//!                                      MarkChunkSent(); }
//!                        consume Fill(...) bytes; }
//!
//! A full chunk is sent only when MORE bytes arrive -- until then it may be
//! the whole file (Close() sends one multipart request) or the final chunk
//! (which must carry the total). So the final chunk is never empty, and a
//! file of exactly one chunk never opens a session.
class ChunkedUploadBuffer {
public:
	enum class CloseAction {
		SINGLE_REQUEST, //!< nothing was sent yet: Pending() is the whole file
		FINAL_CHUNK     //!< a session is open: Pending() is its non-empty last chunk
	};

	//! `chunk_bytes` is normalised (NormalizeDriveChunkSize).
	explicit ChunkedUploadBuffer(std::uint64_t chunk_bytes);

	//! Append as much of [data, data+len) as fits in the chunk; returns the
	//! number of bytes taken (0 only when Full() or len == 0).
	std::size_t Fill(const char *data, std::size_t len);
	//! The pending chunk is complete; another byte needs a send first.
	bool Full() const {
		return pending.size() >= chunk;
	}
	//! The pending chunk was accepted by the server as a non-final chunk.
	void MarkChunkSent();
	CloseAction OnClose() const {
		return sent_any ? CloseAction::FINAL_CHUNK : CloseAction::SINGLE_REQUEST;
	}

	//! Content-Range for Pending() minus its first `skip` bytes (a resume):
	//! "bytes a-b/*" for a non-final chunk (total unknown), "bytes a-b/total"
	//! for the final one.
	std::string ContentRange(bool final, std::size_t skip = 0) const;
	//! Given the next byte the server expects, how many bytes of Pending() it
	//! already holds. False when that offset lies outside the pending chunk:
	//! bytes before it are no longer in memory, so it cannot be resumed.
	bool ResumeSkip(std::uint64_t next_expected, std::size_t &skip) const;

	const std::string &Pending() const {
		return pending;
	}
	std::uint64_t ChunkBytes() const {
		return chunk;
	}
	//! Bytes already sent (the absolute offset of Pending()).
	std::uint64_t Offset() const {
		return offset;
	}
	std::uint64_t Total() const {
		return offset + pending.size();
	}
	bool SessionStarted() const {
		return sent_any;
	}
	//! The most bytes ever held at once -- the memory bound, made observable.
	std::uint64_t Peak() const {
		return peak;
	}

private:
	std::uint64_t chunk;
	std::string pending;
	std::uint64_t offset = 0;
	std::uint64_t peak = 0;
	bool sent_any = false;
};

//! One reply from the session -- to a chunk PUT or to a status query.
struct UploadReply {
	bool transport_ok = false; //!< false: no HTTP response at all
	int status = 0;
	std::string range; //!< the Range header of a 308 ("" when absent)
	//! For an error status: does the error classification say retrying could
	//! help (5xx, 429, rate limit)?
	bool retryable = false;
};

//! Per-chunk progress, carried across NextUploadStep calls for ONE chunk.
struct ChunkUploadState {
	std::size_t skip = 0; //!< bytes of Pending() the session already holds
	int stalls = 0;       //!< consecutive 308s that moved nothing (reset by progress)
	int failures = 0;     //!< errors / transport failures so far
	//! A FINAL-chunk attempt may have reached Drive (transport failure or 5xx
	//! after sending): if this chunk then fails, the outcome is unknown.
	bool final_maybe_sent = false;
};

enum class UploadStepKind {
	DONE,     //!< final chunk: the file is published (body = its metadata)
	ACCEPTED, //!< non-final chunk: Drive holds all of it
	SEND,     //!< send Pending() from state.skip (skip == size: a status query with the total)
	ASK_STATUS, //!< ask the session what it holds (PUT "bytes */*" or "*/total")
	FAIL      //!< give up; `error` says why
};

struct UploadStep {
	UploadStepKind kind = UploadStepKind::SEND;
	bool backoff = false;   //!< sleep (jittered backoff) before the next request
	bool ambiguous = false; //!< FAIL of a final chunk that may have been applied
	std::string error;
};

//! Bound on errors, and on consecutive no-progress 308s, per chunk.
constexpr int kMaxChunkAttempts = 5;

//! The per-chunk retry/resume state machine of a resumable upload, pure: feed
//! each reply in, do what the step says. GDriveClientImpl::UploadChunk is the
//! I/O around it.
UploadStep NextUploadStep(const ChunkedUploadBuffer &buf, bool final, ChunkUploadState &state,
                          const UploadReply &reply);

// ---------------------------------------------------------------------------
// Retry safety of single-request writes (pure). A failure after the request
// was sent -- a dropped connection, a 5xx -- is AMBIGUOUS: Drive may have
// applied it and only the answer was lost. Whether repeating it is safe
// depends on what the request does, not on its HTTP verb:
//
//   * a CREATE makes a new object every time, unless it names an id reserved
//     with files.generateIds -- then a repeat answers 409 "fileIdInUse"
//     instead of making a second file or folder (verified live 2026-11-13,
//     for files, folders, and a resumable session's final chunk alike);
//   * an OVERWRITE (files.update with content, by id) puts the same bytes on
//     the same id again: the end state is the same, at the cost of one more
//     revision;
//   * a MOVE (files.update by id with an explicit name, addParents and
//     removeParents) and a TRASH ({trashed:true}) set absolute state; a
//     repeat answers 200 with the same result (verified live 2026-11-13).
// ---------------------------------------------------------------------------

enum class WriteKind { CREATE, OVERWRITE, MOVE, TRASH };

//! May a write of this kind be re-sent after an ambiguous failure?
bool IsRetrySafe(WriteKind kind, bool has_reserved_id);

//! What a create's FINAL reply -- after the transport's own retries -- means.
enum class CreateOutcome {
	DONE,           //!< 2xx: created, the body is the new object
	ADOPT_RESERVED, //!< 409 fileIdInUse after an unanswered attempt: that attempt landed
	ID_IN_USE,      //!< 409 fileIdInUse with NO unanswered attempt: someone else has our id
	CHECK_RESERVED, //!< no usable answer (transport / 5xx): look the reserved id up
	FAIL_CLEAN,     //!< Drive refused the request: nothing was created
	FAIL_UNKNOWN    //!< no usable answer and no reserved id: outcome unknown
};

//! `reason` is Google's errors[0].reason (GDriveError::reason).
//! `transport_ok` false means no HTTP response at all.
//! `had_ambiguous_attempt`: an EARLIER attempt of this same call went
//! unanswered (transport failure or 5xx) and so may have landed. STRICT: only
//! then is a 409 fileIdInUse ours. Without one, the id was never used by us,
//! so a 409 is a foreign use of a reserved id -- a bug or a concurrent writer
//! -- and must be a loud error, never silently adopted.
CreateOutcome InterpretCreateReply(bool transport_ok, int status, const std::string &reason,
                                   bool has_reserved_id, bool had_ambiguous_attempt);

//! Did a move whose reply was lost land? From the file's own state, read back
//! by id: it carries the target name and sits under the target parent (and,
//! when the move changed parents, no longer under the old one).
bool MoveLanded(const std::string &name, const std::vector<std::string> &parents, const std::string &want_name,
                const std::string &new_parent_id, const std::string &old_parent_id);

} // namespace gdrive
} // namespace duckdb
