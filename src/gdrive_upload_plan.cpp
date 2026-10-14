// PURE: the streaming-write state machine and resumable-session helpers. See
// gdrive_upload_plan.hpp. No duckdb.hpp, no I/O.
#include "gdrive_upload_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <cstdint>
#include <string>

namespace duckdb {
namespace gdrive {

std::uint64_t NormalizeDriveChunkSize(std::uint64_t requested) {
	if (requested > kDriveUploadChunkMax) {
		return kDriveUploadChunkMax;
	}
	std::uint64_t rounded = (requested / kDriveUploadChunkMultiple) * kDriveUploadChunkMultiple;
	return rounded == 0 ? kDriveUploadChunkMultiple : rounded;
}

namespace {

//! Up to 19 decimal digits (always fits in uint64_t); more is refused.
bool ParseDigits(const std::string &s, std::size_t &pos, std::uint64_t &out) {
	const std::size_t start = pos;
	std::uint64_t value = 0;
	while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
		if (pos - start >= 19) {
			return false;
		}
		value = value * 10 + static_cast<std::uint64_t>(s[pos] - '0');
		pos++;
	}
	if (pos == start) {
		return false;
	}
	out = value;
	return true;
}

} // namespace

bool ParseDriveRangeNext(const std::string &range_header, std::uint64_t &next) {
	if (range_header.empty()) {
		next = 0;
		return true;
	}
	static const std::string kPrefix = "bytes=";
	if (range_header.compare(0, kPrefix.size(), kPrefix) != 0) {
		return false;
	}
	std::size_t pos = kPrefix.size();
	std::uint64_t first = 0, last = 0;
	if (!ParseDigits(range_header, pos, first) || pos >= range_header.size() || range_header[pos] != '-') {
		return false;
	}
	pos++;
	if (!ParseDigits(range_header, pos, last) || pos != range_header.size() || last < first) {
		return false;
	}
	// A session holds a prefix of the upload; anything else is not a reply we
	// understand. And last + 1 must not wrap.
	if (first != 0 || last == UINT64_MAX) {
		return false;
	}
	next = last + 1;
	return true;
}

bool DriveSessionPath(const std::string &session_uri, std::string &path_and_query) {
	static const std::string kOrigin = "https://www.googleapis.com/";
	if (session_uri.size() <= kOrigin.size() || session_uri.compare(0, kOrigin.size(), kOrigin) != 0) {
		return false;
	}
	path_and_query = session_uri.substr(kOrigin.size() - 1);
	return true;
}

ChunkedUploadBuffer::ChunkedUploadBuffer(std::uint64_t chunk_bytes) : chunk(NormalizeDriveChunkSize(chunk_bytes)) {
}

std::size_t ChunkedUploadBuffer::Fill(const char *data, std::size_t len) {
	const std::size_t room = static_cast<std::size_t>(chunk - std::min<std::uint64_t>(chunk, pending.size()));
	const std::size_t take = std::min(room, len);
	if (take > 0) {
		const std::size_t needed = pending.size() + take;
		if (needed > pending.capacity()) {
			// Grow geometrically, as the bytes arrive -- a small file costs
			// what it wrote, not a whole chunk -- but never past one chunk.
			const std::size_t doubled = std::max<std::size_t>(needed, 2 * pending.capacity());
			pending.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(chunk, doubled)));
		}
		pending.append(data, take);
		peak = std::max<std::uint64_t>(peak, pending.size());
	}
	return take;
}

void ChunkedUploadBuffer::MarkChunkSent() {
	offset += pending.size();
	pending.clear(); // keeps its capacity: the next chunk reuses the allocation
	sent_any = true;
}

std::string ChunkedUploadBuffer::ContentRange(bool final, std::size_t skip) const {
	const std::uint64_t start = offset + skip;
	const std::uint64_t end = offset + pending.size() - 1;
	return "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
	       (final ? std::to_string(Total()) : std::string("*"));
}

bool ChunkedUploadBuffer::ResumeSkip(std::uint64_t next_expected, std::size_t &skip) const {
	if (next_expected < offset || next_expected > Total()) {
		return false;
	}
	skip = static_cast<std::size_t>(next_expected - offset);
	return true;
}

UploadStep NextUploadStep(const ChunkedUploadBuffer &buf, bool final, ChunkUploadState &state,
                          const UploadReply &reply) {
	UploadStep step;
	auto fail = [&](const std::string &why) {
		step.kind = UploadStepKind::FAIL;
		step.error = why;
		step.ambiguous = final && state.final_maybe_sent;
		return step;
	};
	const std::size_t pending = buf.Pending().size();

	if (reply.transport_ok && (reply.status == 200 || reply.status == 201)) {
		if (!final) {
			return fail("resumable upload: Drive completed the upload before its final chunk");
		}
		step.kind = UploadStepKind::DONE;
		return step;
	}

	if (reply.transport_ok && reply.status == 308) {
		std::uint64_t next = 0;
		if (!ParseDriveRangeNext(reply.range, next)) {
			return fail("resumable upload: Drive answered with a Range header that is not \"bytes=0-N\"");
		}
		std::size_t skip = 0;
		if (!buf.ResumeSkip(next, skip)) {
			return fail("resumable upload: Drive reports holding bytes outside the chunk still in memory; the "
			            "upload cannot be resumed");
		}
		if (skip > state.skip || (skip == pending && !final)) {
			state.stalls = 0;
		} else if (++state.stalls > kMaxChunkAttempts) {
			return fail("resumable upload made no progress after repeated attempts");
		}
		step.backoff = state.stalls > 0;
		state.skip = skip;
		if (skip == pending && !final) {
			step.kind = UploadStepKind::ACCEPTED;
			return step;
		}
		if (!final && skip < pending && (buf.Offset() + skip) % kDriveUploadChunkMultiple != 0) {
			// The rest would be a non-final chunk not on a 256 KiB boundary,
			// which Drive refuses with an opaque 400. Say what happened.
			return fail("resumable upload: Drive acknowledged a non-final chunk up to a byte that is not a "
			            "multiple of 256 KiB; the rest cannot be sent as a valid chunk");
		}
		step.kind = UploadStepKind::SEND; // skip == pending on a final chunk: query with the total
		return step;
	}

	// No response, or an error status.
	const bool retryable = !reply.transport_ok || reply.retryable;
	if (final && (!reply.transport_ok || reply.status >= 500)) {
		state.final_maybe_sent = true;
	}
	if (retryable && ++state.failures <= kMaxChunkAttempts) {
		step.kind = UploadStepKind::ASK_STATUS;
		step.backoff = true;
		return step;
	}
	if (!reply.transport_ok) {
		return fail("network transport failure uploading a chunk (no response from Drive)");
	}
	return fail("resumable upload chunk rejected with HTTP " + std::to_string(reply.status));
}

} // namespace gdrive
} // namespace duckdb
