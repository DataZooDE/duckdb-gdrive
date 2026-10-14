#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

} // namespace gdrive
} // namespace duckdb
