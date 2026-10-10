// GDriveBlockCache -- see the contract in gdrive_blockcache.hpp. PURE.
#include "gdrive_blockcache.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace duckdb {
namespace gdrive {

void GDriveBlockCache::SetCapacity(uint64_t bytes) {
	std::lock_guard<std::mutex> guard(lock);
	capacity_bytes = bytes;
	EvictLocked();
}

uint64_t GDriveBlockCache::BytesCached() {
	std::lock_guard<std::mutex> guard(lock);
	return cached_bytes;
}

void GDriveBlockCache::Clear() {
	std::lock_guard<std::mutex> guard(lock);
	blocks.clear();
	cached_bytes = 0;
}

//! Least-recently-used eviction. Caller holds `lock`.
//!
//! Only entries whose fetch has COMPLETED are evictable: dropping an in-flight
//! entry would let a second thread start a duplicate request for the same
//! block, which is the one thing this cache exists to prevent. A block still
//! held by a reader stays alive through its shared_ptr regardless.
void GDriveBlockCache::EvictLocked() {
	if (capacity_bytes == 0) {
		blocks.clear();
		cached_bytes = 0;
		return;
	}
	while (cached_bytes > capacity_bytes) {
		auto victim = blocks.end();
		for (auto it = blocks.begin(); it != blocks.end(); ++it) {
			if (it->second.value.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				continue; // in flight
			}
			if (victim == blocks.end() || it->second.used_at < victim->second.used_at) {
				victim = it;
			}
		}
		if (victim == blocks.end()) {
			return; // everything in flight; let the next insert try again
		}
		cached_bytes -= victim->second.bytes;
		blocks.erase(victim);
	}
}

std::shared_ptr<const std::string> GDriveBlockCache::GetBlock(const std::string &key, uint64_t block_index,
                                                              uint64_t block_size, uint64_t file_size,
                                                              const Fetch &fetch) {
	std::string full_key = key;
	full_key += '\x1f';
	full_key += std::to_string(block_size);
	full_key += '\x1f';
	full_key += std::to_string(block_index);

	std::shared_future<std::shared_ptr<const std::string>> future;
	std::promise<std::shared_ptr<const std::string>> promise;
	bool i_fetch = false;
	uint64_t my_generation = 0;

	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = blocks.find(full_key);
		if (it != blocks.end()) {
			it->second.used_at = ++clock;
			future = it->second.value;
		} else {
			// Insert the FUTURE before releasing the lock, so a second thread
			// arriving for the same block waits on this fetch instead of
			// starting its own. With 18 threads scanning one file, the
			// difference is 1 request versus 18 identical ones.
			future = promise.get_future().share();
			Entry entry;
			entry.value = future;
			entry.used_at = ++clock;
			entry.generation = my_generation = ++next_generation;
			blocks.emplace(full_key, entry);
			i_fetch = true;
		}
	}

	if (i_fetch) {
		const uint64_t start = block_index * block_size;
		const uint64_t len = std::min<uint64_t>(block_size, file_size > start ? file_size - start : 0);
		try {
			auto data = std::make_shared<std::string>();
			fetch(start, len, *data);
			std::shared_ptr<const std::string> stored = std::move(data);
			{
				std::lock_guard<std::mutex> guard(lock);
				auto it = blocks.find(full_key);
				if (it != blocks.end() && it->second.generation == my_generation) {
					it->second.bytes = stored->size();
					cached_bytes += stored->size();
				}
				EvictLocked();
			}
			promise.set_value(stored);
			return stored;
		} catch (...) {
			// A failed fetch must not be cached, and every waiter must see the
			// error rather than hang.
			{
				std::lock_guard<std::mutex> guard(lock);
				auto it = blocks.find(full_key);
				if (it != blocks.end() && it->second.generation == my_generation) {
					cached_bytes -= it->second.bytes;
					blocks.erase(it);
				}
			}
			promise.set_exception(std::current_exception());
			throw;
		}
	}

	return future.get();
}

} // namespace gdrive
} // namespace duckdb
