// PURE: the streaming-write state machine and resumable-session helpers. See
// gdrive_upload_plan.hpp. No duckdb.hpp, no I/O.
#include "gdrive_upload_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

bool ParseDigits(const std::string &s, std::size_t &pos, std::uint64_t &out) {
	const std::size_t start = pos;
	while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
		pos++;
	}
	if (pos == start) {
		return false;
	}
	out = std::strtoull(s.substr(start, pos - start).c_str(), nullptr, 10);
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
		if (pending.capacity() < chunk) {
			// One allocation of exactly one chunk, never grown past it.
			pending.reserve(static_cast<std::size_t>(chunk));
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

} // namespace gdrive
} // namespace duckdb
