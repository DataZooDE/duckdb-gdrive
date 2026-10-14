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
	// A session holds a PREFIX of the upload: a range not starting at 0 is
	// not something Drive sends, and must not be half-trusted.
	REQUIRE_FALSE(ParseDriveRangeNext("bytes=5-10", next));
	// Overflow: last + 1 must not wrap to 0, and huge digit runs are refused.
	REQUIRE_FALSE(ParseDriveRangeNext("bytes=0-18446744073709551615", next));
	REQUIRE_FALSE(ParseDriveRangeNext("bytes=0-99999999999999999999999", next));
}

// ---------------------------------------------------------------------------
// NextUploadStep: the per-chunk retry/resume state machine, pure. Each reply
// (to a chunk PUT or a status query) is fed in; the step says what to do next.
// Reply shapes are the ones observed live (308 + "Range: bytes=0-N", 200/201
// on completion, 5xx/429 retryable, 404 once a session is gone).
// ---------------------------------------------------------------------------
namespace {

ChunkedUploadBuffer BufferWithPending(std::uint64_t full_chunks_sent, std::size_t pending) {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer buf(C);
	std::string data(static_cast<std::size_t>(C), 'x');
	for (std::uint64_t i = 0; i < full_chunks_sent; i++) {
		buf.Fill(data.data(), data.size());
		buf.MarkChunkSent();
	}
	std::string rest(pending, 'y');
	buf.Fill(rest.data(), rest.size());
	return buf;
}

UploadReply Reply(int status, std::string range = "", bool retryable = false) {
	UploadReply r;
	r.transport_ok = true;
	r.status = status;
	r.range = std::move(range);
	r.retryable = retryable;
	return r;
}

UploadReply TransportFailure() {
	UploadReply r;
	r.transport_ok = false;
	return r;
}

} // namespace

TEST_CASE("upload step: a non-final chunk acknowledged in full is accepted", "[upload][step]") {
	auto buf = BufferWithPending(1, 262144); // offset 262144, one full chunk pending
	ChunkUploadState st;
	auto step = NextUploadStep(buf, false, st, Reply(308, "bytes=0-524287"));
	REQUIRE(step.kind == UploadStepKind::ACCEPTED);
}

TEST_CASE("upload step: the final chunk completing is done", "[upload][step]") {
	auto buf = BufferWithPending(1, 100);
	ChunkUploadState st;
	REQUIRE(NextUploadStep(buf, true, st, Reply(200)).kind == UploadStepKind::DONE);
	REQUIRE(NextUploadStep(buf, true, st, Reply(201)).kind == UploadStepKind::DONE);
}

TEST_CASE("upload step: a partial 308 re-sends the rest and progress resets the stall count", "[upload][step]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer big(4 * C);
	std::string data(static_cast<std::size_t>(4 * C), 'x');
	big.Fill(data.data(), data.size());
	ChunkUploadState st;
	// Drive took the first 256 KiB only: send the rest, no backoff.
	auto step = NextUploadStep(big, false, st, Reply(308, "bytes=0-262143"));
	REQUIRE(step.kind == UploadStepKind::SEND);
	REQUIRE_FALSE(step.backoff);
	REQUIRE(st.skip == C);
	// No progress this time: send again, but back off first.
	step = NextUploadStep(big, false, st, Reply(308, "bytes=0-262143"));
	REQUIRE(step.kind == UploadStepKind::SEND);
	REQUIRE(step.backoff);
	REQUIRE(st.stalls == 1);
	// Progress again: the stall count resets, so a slow-but-moving session
	// is never aborted for "no progress".
	step = NextUploadStep(big, false, st, Reply(308, "bytes=0-524287"));
	REQUIRE(step.kind == UploadStepKind::SEND);
	REQUIRE(st.stalls == 0);
	REQUIRE(st.skip == 2 * C);
}

TEST_CASE("upload step: a session that never progresses fails after the bound", "[upload][step]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer big(2 * C);
	std::string data(static_cast<std::size_t>(2 * C), 'x');
	big.Fill(data.data(), data.size());
	ChunkUploadState st;
	UploadStep step;
	for (int i = 0; i < 20 && step.kind != UploadStepKind::FAIL; i++) {
		step = NextUploadStep(big, false, st, Reply(308, ""));
	}
	REQUIRE(step.kind == UploadStepKind::FAIL);
	REQUIRE(step.error.find("no progress") != std::string::npos);
}

TEST_CASE("upload step: an unaligned resume of a non-final chunk is a clear error", "[upload][step]") {
	const std::uint64_t C = kDriveUploadChunkMultiple;
	ChunkedUploadBuffer big(2 * C);
	std::string data(static_cast<std::size_t>(2 * C), 'x');
	big.Fill(data.data(), data.size());
	ChunkUploadState st;
	auto step = NextUploadStep(big, false, st, Reply(308, "bytes=0-999"));
	REQUIRE(step.kind == UploadStepKind::FAIL);
	REQUIRE(step.error.find("256 KiB") != std::string::npos);
	// The FINAL chunk may resume anywhere: it carries the total.
	ChunkUploadState st2;
	step = NextUploadStep(big, true, st2, Reply(308, "bytes=0-999"));
	REQUIRE(step.kind == UploadStepKind::SEND);
	REQUIRE(st2.skip == 1000);
}

TEST_CASE("upload step: a malformed or out-of-chunk Range is a hard failure", "[upload][step]") {
	auto buf = BufferWithPending(1, 100); // offset 262144
	ChunkUploadState st;
	REQUIRE(NextUploadStep(buf, false, st, Reply(308, "bytes=garbage")).kind == UploadStepKind::FAIL);
	ChunkUploadState st2;
	// Drive holds less than what was already dropped from memory.
	REQUIRE(NextUploadStep(buf, false, st2, Reply(308, "bytes=0-99")).kind == UploadStepKind::FAIL);
	ChunkUploadState st3;
	// ...or more than was ever sent.
	REQUIRE(NextUploadStep(buf, true, st3, Reply(308, "bytes=0-999999")).kind == UploadStepKind::FAIL);
}

TEST_CASE("upload step: a retryable error backs off, then asks the session", "[upload][step]") {
	auto buf = BufferWithPending(1, 100);
	ChunkUploadState st;
	auto step = NextUploadStep(buf, false, st, Reply(503, "", /*retryable=*/true));
	REQUIRE(step.kind == UploadStepKind::ASK_STATUS);
	REQUIRE(step.backoff);
	step = NextUploadStep(buf, false, st, TransportFailure());
	REQUIRE(step.kind == UploadStepKind::ASK_STATUS);
	// ...bounded.
	for (int i = 0; i < 10 && step.kind == UploadStepKind::ASK_STATUS; i++) {
		step = NextUploadStep(buf, false, st, TransportFailure());
	}
	REQUIRE(step.kind == UploadStepKind::FAIL);
}

TEST_CASE("upload step: a non-retryable error fails at once", "[upload][step]") {
	auto buf = BufferWithPending(1, 100);
	ChunkUploadState st;
	auto step = NextUploadStep(buf, false, st, Reply(404));
	REQUIRE(step.kind == UploadStepKind::FAIL);
	REQUIRE_FALSE(step.ambiguous);
}

TEST_CASE("upload step: a final chunk whose fate is unknown fails as ambiguous", "[upload][step]") {
	// The final PUT may have reached Drive and published the file even though
	// its reply was lost; if every later attempt fails too, the error must say
	// so rather than claim nothing happened.
	auto buf = BufferWithPending(1, 100);
	ChunkUploadState st;
	UploadStep step = NextUploadStep(buf, true, st, TransportFailure());
	REQUIRE(step.kind == UploadStepKind::ASK_STATUS);
	for (int i = 0; i < 10 && step.kind == UploadStepKind::ASK_STATUS; i++) {
		step = NextUploadStep(buf, true, st, TransportFailure());
	}
	REQUIRE(step.kind == UploadStepKind::FAIL);
	REQUIRE(step.ambiguous);
	// A lost final reply that the status query resolves is plain success.
	ChunkUploadState st2;
	REQUIRE(NextUploadStep(buf, true, st2, TransportFailure()).kind == UploadStepKind::ASK_STATUS);
	REQUIRE(NextUploadStep(buf, true, st2, Reply(200)).kind == UploadStepKind::DONE);
	// A definite refusal of a NON-final chunk is never ambiguous.
	ChunkUploadState st3;
	REQUIRE_FALSE(NextUploadStep(buf, false, st3, Reply(400)).ambiguous);
}

TEST_CASE("upload step: the whole final chunk held but not completed asks for completion", "[upload][step]") {
	auto buf = BufferWithPending(1, 100); // total 262244
	ChunkUploadState st;
	auto step = NextUploadStep(buf, true, st, Reply(308, "bytes=0-262243"));
	REQUIRE(step.kind == UploadStepKind::SEND);
	REQUIRE(st.skip == 100); // caller sends the status query with the total
}

TEST_CASE("streaming upload: a small file does not allocate a whole chunk", "[upload][streaming]") {
	ChunkedUploadBuffer buf(kDriveUploadChunkMax);
	const char one = 'x';
	REQUIRE(buf.Fill(&one, 1) == 1);
	REQUIRE(buf.Pending().capacity() < 4096);
	std::string some(100000, 'y');
	REQUIRE(buf.Fill(some.data(), some.size()) == some.size());
	REQUIRE(buf.Pending().capacity() < 4 * 100001);
}

TEST_CASE("streaming upload: content range of a resumed chunk", "[upload][streaming]") {
	auto buf = BufferWithPending(1, 100);
	REQUIRE(buf.ContentRange(false, 40) == "bytes 262184-262243/*");
	REQUIRE(buf.ContentRange(true, 40) == "bytes 262184-262243/262244");
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
