// Catch2: the streaming-write state machine (gdrive_upload_plan.*). Pure: no
// duckdb.hpp, no I/O. The resumable-session PUT loop that drives it is proven
// live (test/sql/gdrive_streaming_write.test.template, e2e).
#include "gdrive_upload_plan.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace duckdb::gdrive;

namespace {

//! Drive a ChunkedUploadBuffer the way GDriveFileSystem::Write does and
//! record each non-final chunk's Content-Range. Returns the close action.
ChunkedUploadBuffer::CloseAction Stream(ChunkedUploadBuffer &buf, const std::vector<std::size_t> &writes,
                                        std::vector<std::string> &flushed) {
	for (auto n : writes) {
		std::string data(n, 'x');
		const char *p = data.data();
		std::size_t left = n;
		while (left > 0) {
			if (buf.Full()) {
				flushed.push_back(buf.ContentRange(false));
				REQUIRE(buf.Pending().size() == buf.ChunkBytes());
				buf.MarkChunkSent();
			}
			auto took = buf.Fill(p, left);
			REQUIRE(took > 0);
			p += took;
			left -= took;
			REQUIRE(buf.Pending().size() <= buf.ChunkBytes());
		}
	}
	return buf.OnClose();
}

} // namespace

TEST_CASE("drive upload chunk size rounds down to a 256 KiB multiple", "[upload]") {
	REQUIRE(kDriveUploadChunkMultiple == 262144);
	REQUIRE(NormalizeDriveChunkSize(16ull * 1024 * 1024) == 16ull * 1024 * 1024);
	REQUIRE(NormalizeDriveChunkSize(kDriveUploadChunkMultiple * 3 + 5) == kDriveUploadChunkMultiple * 3);
	// Below one multiple clamps up, never to zero: Drive rejects a non-final
	// chunk under 256 KiB ("required to be equal or greater than 262144").
	REQUIRE(NormalizeDriveChunkSize(0) == kDriveUploadChunkMultiple);
	REQUIRE(NormalizeDriveChunkSize(1) == kDriveUploadChunkMultiple);
	// Bounded: the chunk IS the handle's memory footprint.
	REQUIRE(NormalizeDriveChunkSize(1ull << 40) == kDriveUploadChunkMax);
	REQUIRE(kDriveUploadChunkMax % kDriveUploadChunkMultiple == 0);
}

TEST_CASE("streaming upload: small files stay a single request", "[upload][streaming]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	for (std::uint64_t size : {std::uint64_t(0), std::uint64_t(1), C - 1, C}) {
		INFO(size);
		ChunkedUploadBuffer buf(C);
		std::vector<std::string> flushed;
		auto action = Stream(buf, {static_cast<std::size_t>(size)}, flushed);
		REQUIRE(flushed.empty());
		REQUIRE(action == ChunkedUploadBuffer::CloseAction::SINGLE_REQUEST);
		REQUIRE_FALSE(buf.SessionStarted());
		REQUIRE(buf.Total() == size);
	}
}

TEST_CASE("streaming upload: large files flush full chunks as they fill", "[upload][streaming]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer buf(C);
	std::vector<std::string> flushed;
	auto action = Stream(buf, {1000, static_cast<std::size_t>(C), static_cast<std::size_t>(2 * C - 1000), 17},
	                     flushed);
	REQUIRE(action == ChunkedUploadBuffer::CloseAction::FINAL_CHUNK);
	REQUIRE(buf.SessionStarted());
	REQUIRE(flushed ==
	        std::vector<std::string> {"bytes 0-262143/*", "bytes 262144-524287/*", "bytes 524288-786431/*"});
	REQUIRE(buf.Offset() == 3 * C);
	REQUIRE(buf.Total() == 3 * C + 17);
	REQUIRE(buf.ContentRange(true) == "bytes 786432-786448/786449");
	REQUIRE(buf.Peak() == C);
}

TEST_CASE("streaming upload: an exact multiple keeps its last chunk for close", "[upload][streaming]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer buf(C);
	std::vector<std::string> flushed;
	auto action = Stream(buf, {static_cast<std::size_t>(2 * C)}, flushed);
	REQUIRE(flushed.size() == 1);
	REQUIRE(action == ChunkedUploadBuffer::CloseAction::FINAL_CHUNK);
	REQUIRE(buf.ContentRange(true) == "bytes 262144-524287/524288");
}

TEST_CASE("streaming upload: one huge write is still chunked, not buffered whole", "[upload][streaming]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer buf(C);
	std::vector<std::string> flushed;
	Stream(buf, {static_cast<std::size_t>(10 * C + 5)}, flushed);
	REQUIRE(flushed.size() == 10);
	REQUIRE(buf.Peak() == C);
}

TEST_CASE("streaming upload: a resume offset must fall inside the pending chunk", "[upload][streaming]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer buf(C);
	std::vector<std::string> flushed;
	Stream(buf, {static_cast<std::size_t>(C + 100)}, flushed);
	std::size_t skip = 999;
	REQUIRE(buf.ResumeSkip(C, skip));
	REQUIRE(skip == 0);
	REQUIRE(buf.ResumeSkip(C + 40, skip));
	REQUIRE(skip == 40);
	REQUIRE(buf.ResumeSkip(C + 100, skip));
	REQUIRE(skip == 100);
	REQUIRE_FALSE(buf.ResumeSkip(C - 1, skip));
	REQUIRE_FALSE(buf.ResumeSkip(C + 101, skip));
}

TEST_CASE("drive 308 Range header gives the next byte to send", "[upload]") {
	std::uint64_t next = 999;
	REQUIRE(ParseDriveRangeNext("bytes=0-262143", next));
	REQUIRE(next == 262144);
	// No Range header on a 308: Drive has nothing yet.
	REQUIRE(ParseDriveRangeNext("", next));
	REQUIRE(next == 0);
	REQUIRE_FALSE(ParseDriveRangeNext("bytes=5-", next));
	REQUIRE_FALSE(ParseDriveRangeNext("garbage", next));
	REQUIRE_FALSE(ParseDriveRangeNext("bytes=10-4", next));
}

TEST_CASE("drive resumable session URIs are only followed to the Drive upload host", "[upload]") {
	std::string path;
	REQUIRE(DriveSessionPath("https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&upload_id=x",
	                         path));
	REQUIRE(path == "/upload/drive/v3/files?uploadType=resumable&upload_id=x");
	// The session URI is a bearer credential for the upload: never to another
	// host, never over plain http.
	REQUIRE_FALSE(DriveSessionPath("http://www.googleapis.com/upload/drive/v3/files?upload_id=x", path));
	REQUIRE_FALSE(DriveSessionPath("https://evil.example.com/upload?upload_id=x", path));
	REQUIRE_FALSE(DriveSessionPath("https://www.googleapis.com.evil.com/upload", path));
	REQUIRE_FALSE(DriveSessionPath("", path));
}
