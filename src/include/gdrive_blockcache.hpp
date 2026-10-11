#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace duckdb {
namespace gdrive {

//! ---------------------------------------------------------------------------
//! Shared block cache for file CONTENT. PURE (no duckdb.hpp, no I/O: the fetch
//! is injected), so test/cpp/test_blockcache.cpp drives it directly -- the
//! same split as the sibling duckdb-sharepoint's SharepointBlockCache.
//!
//! Why a shared cache and not per-handle read-ahead: measured, one Parquet
//! scan issued 35 ranged GETs across 18 handles, and although half of them
//! began exactly where another ended, NO adjacent pair shared a handle. A
//! per-handle buffer therefore never sees the follow-on read. That version was
//! built, measured (136 MB fetched instead of 54.9 MB, request count
//! unchanged) and reverted -- see docs/benchmark.md.
//!
//! Why blocks at all: Drive's media endpoint costs ~1.2 s per request
//! REGARDLESS OF SIZE. A 1 KB read and a 1 MB read cost the same; the whole
//! 87 MB file in one request costs 2.06 s. So the winning move on Drive is
//! the opposite of the usual one -- fetch MORE in FEWER requests.
//!
//! Keyed by identity + file id + headRevisionId (the caller's `key`) + block
//! size + block index:
//!   * identity, because one FileSystem object serves every ClientContext and
//!     a content cache keyed by file id alone would hand one tenant another's
//!     bytes. (The token cache shipped exactly that bug once.)
//!   * headRevisionId, because Drive keeps the file id across an overwrite --
//!     without it, rewritten content would be served from a stale block.
//!   * block size, because block N spans different bytes at a different size,
//!     and gdrive_block_size_bytes is a per-session setting: without it a
//!     session reading with 16 MiB blocks was handed another session's 1 MiB
//!     block 0 and failed with a spurious "short read".
//!
//! Concurrent readers of the same block share ONE fetch via a shared_future:
//! 18 threads hitting a cold block must not issue 18 identical requests.
//! ---------------------------------------------------------------------------
class GDriveBlockCache {
public:
	//! (start, length, out): fill `out` with exactly the bytes of the block.
	using Fetch = std::function<void(uint64_t, uint64_t, std::string &)>;

	//! The block containing `block_index`, fetching it if absent. `fetch` runs
	//! at most once per block no matter how many threads ask concurrently; if
	//! it throws, nothing is cached and every waiter sees the exception.
	std::shared_ptr<const std::string> GetBlock(const std::string &key, uint64_t block_index, uint64_t block_size,
	                                            uint64_t file_size, const Fetch &fetch);
	void SetCapacity(uint64_t bytes);
	void Clear();
	uint64_t BytesCached();

private:
	std::mutex lock;
	struct Entry {
		std::shared_future<std::shared_ptr<const std::string>> value;
		uint64_t bytes = 0;
		uint64_t used_at = 0;
		//! Which insertion this is. A fetch that completes after its entry was
		//! cleared (and possibly re-created by another thread) must account to,
		//! or erase, only ITS OWN entry -- never a newer one under the same key.
		uint64_t generation = 0;
	};
	std::unordered_map<std::string, Entry> blocks;
	uint64_t capacity_bytes = 0;
	uint64_t cached_bytes = 0;
	uint64_t clock = 0;
	uint64_t next_generation = 0;

	//! Caller must hold `lock`.
	void EvictLocked();
};

} // namespace gdrive
} // namespace duckdb
