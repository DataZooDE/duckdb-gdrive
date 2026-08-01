#include "gdrive_trace.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace duckdb {
namespace gdrive {
namespace trace {

namespace {

//! Where records go. Opened once, on the first Emit, under the same mutex that
//! serialises writes.
//!
//! stderr is the fallback rather than the default-and-only, because a trace of
//! a parallel scan interleaved with DuckDB's own stderr output is painful to
//! parse and easy to truncate. GDRIVE_TRACE_FILE is the intended path.
FILE *g_out = nullptr;
std::mutex g_mutex;

//! The process-relative zero point. Captured on the first NowMicros() call,
//! which the guard in Enabled() ensures only happens when tracing is on.
const std::chrono::steady_clock::time_point &Epoch() {
	static const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
	return epoch;
}

//! Written once as the first record, so a trace file is self-describing: the
//! spans carry relative microseconds and this says what they are relative to.
void WriteHeaderLocked() {
	const auto wall = std::chrono::system_clock::now().time_since_epoch();
	const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(wall).count();
	fprintf(g_out, "{\"record\":\"header\",\"version\":1,\"epoch_unix_us\":%lld}\n", (long long)wall_us);
}

} // namespace

bool Enabled() {
	// Function-local static: initialised exactly once, thread-safely, on first
	// use. After that this is a load and a predictable branch -- see the
	// header's note on cost.
	static const bool enabled = [] {
		if (getenv("GDRIVE_TRACE_FILE") != nullptr) {
			return true;
		}
		const char *v = getenv("GDRIVE_TRACE");
		return v != nullptr && v[0] != '\0' && v[0] != '0';
	}();
	return enabled;
}

uint64_t NowMicros() {
	const auto delta = std::chrono::steady_clock::now() - Epoch();
	return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(delta).count();
}

int ThreadId() {
	// A dense counter rather than a hash of std::thread::id: the trace is
	// plotted with one row per thread, and 0..17 makes a readable axis where
	// 140234981238784 does not.
	static std::atomic<int> next {0};
	static thread_local const int id = next.fetch_add(1, std::memory_order_relaxed);
	return id;
}

void Emit(const Span &s) {
	const int thread_id = ThreadId();
	std::lock_guard<std::mutex> guard(g_mutex);
	if (g_out == nullptr) {
		const char *path = getenv("GDRIVE_TRACE_FILE");
		if (path != nullptr && path[0] != '\0') {
			// Plain "w". The glibc "e" (O_CLOEXEC) mode extension is not
			// standard C and is not accepted by MSVC's CRT, where an unknown
			// mode character is undefined behaviour rather than ignored --
			// this file has to build on Linux (glibc and musl), macOS and
			// Windows. Close-on-exec is not worth a platform ifdef here: the
			// extension does not fork or exec, and the descriptor holds a
			// diagnostic trace, not a credential.
			g_out = fopen(path, "w");
		}
		if (g_out == nullptr) {
			g_out = stderr;
		} else {
			// Big buffer, and flush at exit rather than per record. A record is
			// written after its request completes, so a per-record fflush would
			// land on the worker thread between two Drive calls and inflate the
			// gap this trace exists to measure. Buffering makes the observer
			// cheap; atexit makes it durable enough for a benchmark run.
			setvbuf(g_out, nullptr, _IOFBF, 1 << 20);
			atexit([] {
				std::lock_guard<std::mutex> g(g_mutex);
				if (g_out != nullptr) {
					fflush(g_out);
				}
			});
		}
		WriteHeaderLocked();
	}

	// One fwrite per record, under the mutex, so records from concurrent
	// threads never interleave mid-line. A torn line would break the parser
	// for the exact workload this exists to observe.
	//
	// NOTE: nothing here prints a URL, a header or a body. `kind` is one of a
	// fixed set of literals and the range fields are integers, so a trace file
	// cannot contain a bearer token or a file name (REQ-NF-03). File IDs are
	// deliberately omitted too -- they are not secret, but they are the one
	// field that would make a shared trace identify someone's Drive contents.
	fprintf(g_out,
	        "{\"kind\":\"%s\",\"t0_us\":%llu,\"t1_us\":%llu,\"dur_us\":%llu,"
	        "\"thread\":%d,\"attempt\":%d,\"status\":%d,\"bytes\":%zu,"
	        "\"fresh_conn\":%s,\"range_off\":%lld,\"range_len\":%lld}\n",
	        s.kind, (unsigned long long)s.start_us, (unsigned long long)s.end_us,
	        (unsigned long long)(s.end_us - s.start_us), thread_id, s.attempt, s.http_status, s.bytes,
	        s.fresh_conn ? "true" : "false", (long long)s.range_off, (long long)s.range_len);
}

} // namespace trace
} // namespace gdrive
} // namespace duckdb
