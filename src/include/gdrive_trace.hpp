// Span tracing for Drive API calls -- OFF unless explicitly enabled.
//
// Why this exists. docs/benchmark.md's most useful finding came from a trace,
// not from a benchmark: 35 ranged GETs looked eminently coalescable until the
// trace showed no two adjacent ranges shared a file handle, which killed a
// read-ahead buffer that an external review had recommended. Per-request
// medians cannot show that. Neither can they show which requests overlap,
// which is what actually determines wall-clock time when the per-request floor
// is above a second and eighteen handles are in flight.
//
// So this emits one record per Drive API ATTEMPT with enough to reconstruct a
// timeline: when it started, when it ended, on which thread, what kind of call
// it was, whether it paid for a new connection, and whether it was a retry.
//
// COST WHEN DISABLED. `Enabled()` is a function-local `static const bool`
// initialised once from the environment. Every call site is
// `if (trace::Enabled())`, which after the first call is a load of an already-
// initialised flag plus a correctly-predicted branch -- no allocation, no
// clock read, no formatting. The clock reads themselves are also inside the
// guard, so a normal query does not even call steady_clock. This matters: a
// scan issues these on the hot path.
//
// NOT phase-split. httplib does not expose DNS/TCP/TLS/TTFB boundaries, so a
// span here is one duration, not six. The phase breakdown is measured
// separately by e2e/helpers/latency_breakdown.py against the same endpoints.
// Do not add fake phase splits to this file to make the two look alike.

#pragma once

#include <cstddef>
#include <cstdint>

namespace duckdb {
namespace gdrive {
namespace trace {

//! True iff GDRIVE_TRACE_FILE or GDRIVE_TRACE is set in the environment.
//! Read once, at first use. Changing the variable mid-process has no effect,
//! which is deliberate -- a flag that can flip underneath a running scan would
//! produce a trace with holes in it.
bool Enabled();

//! Microseconds since the trace subsystem was first touched. A relative clock
//! keeps the numbers small and makes two runs directly comparable; absolute
//! wall time is recorded once in the header record instead.
uint64_t NowMicros();

//! Small dense per-thread id (0, 1, 2, ...), assigned on first use. Nicer to
//! plot than a hashed std::thread::id, and stable for the thread's lifetime.
int ThreadId();

struct Span {
	const char *kind = "unknown"; //!< files.list, files.get, files.media, ...
	uint64_t start_us = 0;
	uint64_t end_us = 0;
	int attempt = 1;         //!< 1 = first try; >1 means this is a retry
	int http_status = 0;     //!< 0 = transport failure, no response
	size_t bytes = 0;        //!< response body length
	bool fresh_conn = false; //!< paid DNS + TCP + TLS on this thread
	int64_t range_off = -1;  //!< -1 when the request carried no Range
	int64_t range_len = -1;
};

//! Append one JSONL record. Serialised across threads; only ever called when
//! Enabled() is true.
void Emit(const Span &s);

} // namespace trace
} // namespace gdrive
} // namespace duckdb
