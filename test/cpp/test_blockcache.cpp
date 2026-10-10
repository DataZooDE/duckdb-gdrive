// GDriveBlockCache -- pure, so every property the filesystem relies on is
// pinned here with a synthetic fetcher (no Drive, no DuckDB).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gdrive_blockcache.hpp"

using duckdb::gdrive::GDriveBlockCache;

namespace {

const std::string kFile = "0123456789abcdefghijklmnopqrstuvwxyz"; // 36 bytes

GDriveBlockCache::Fetch Counting(std::atomic<int> &fetches) {
	return [&fetches](uint64_t start, uint64_t len, std::string &out) {
		fetches++;
		out = kFile.substr(start, len);
	};
}

} // namespace

TEST_CASE("GDriveBlockCache: a block is fetched once and then served", "[blockcache]") {
	GDriveBlockCache cache;
	cache.SetCapacity(1024);
	std::atomic<int> fetches {0};
	auto b0 = cache.GetBlock("id|rev", 0, 10, kFile.size(), Counting(fetches));
	auto again = cache.GetBlock("id|rev", 0, 10, kFile.size(), Counting(fetches));
	REQUIRE(*b0 == "0123456789");
	REQUIRE(*again == "0123456789");
	REQUIRE(fetches == 1);
	// The final block is short.
	REQUIRE(*cache.GetBlock("id|rev", 3, 10, kFile.size(), Counting(fetches)) == "uvwxyz");
	REQUIRE(fetches == 2);
	REQUIRE(cache.BytesCached() == 16);
}

TEST_CASE("GDriveBlockCache: block size is part of the key", "[blockcache]") {
	// gdrive_block_size_bytes is per session. Block 0 at 4 bytes and block 0 at
	// 10 bytes are different byte ranges; serving one for the other made the
	// larger-block reader fail with a spurious "short read".
	GDriveBlockCache cache;
	cache.SetCapacity(1024);
	std::atomic<int> fetches {0};
	REQUIRE(*cache.GetBlock("id|rev", 0, 4, kFile.size(), Counting(fetches)) == "0123");
	REQUIRE(*cache.GetBlock("id|rev", 0, 10, kFile.size(), Counting(fetches)) == "0123456789");
	REQUIRE(fetches == 2);
}

TEST_CASE("GDriveBlockCache: a failed fetch caches nothing and rethrows", "[blockcache]") {
	GDriveBlockCache cache;
	cache.SetCapacity(1024);
	bool fail = true;
	int fetches = 0;
	auto fetch = [&](uint64_t start, uint64_t len, std::string &out) {
		fetches++;
		if (fail) {
			throw std::runtime_error("503");
		}
		out = kFile.substr(start, len);
	};
	REQUIRE_THROWS(cache.GetBlock("id|rev", 0, 10, kFile.size(), fetch));
	REQUIRE(cache.BytesCached() == 0);
	fail = false;
	REQUIRE(*cache.GetBlock("id|rev", 0, 10, kFile.size(), fetch) == "0123456789");
	REQUIRE(fetches == 2);
}

TEST_CASE("GDriveBlockCache: concurrent readers of one block share one fetch", "[blockcache][concurrency]") {
	GDriveBlockCache cache;
	cache.SetCapacity(1024);
	std::atomic<int> fetches {0};
	auto slow = [&](uint64_t start, uint64_t len, std::string &out) {
		fetches++;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		out = kFile.substr(start, len);
	};
	std::vector<std::thread> threads;
	std::atomic<int> ok {0};
	for (int i = 0; i < 8; i++) {
		threads.emplace_back([&] {
			if (*cache.GetBlock("id|rev", 1, 10, kFile.size(), slow) == "abcdefghij") {
				ok++;
			}
		});
	}
	for (auto &t : threads) {
		t.join();
	}
	REQUIRE(ok == 8);
	REQUIRE(fetches == 1);
}

TEST_CASE("GDriveBlockCache: LRU eviction keeps the cache within capacity", "[blockcache]") {
	GDriveBlockCache cache;
	cache.SetCapacity(20); // two 10-byte blocks
	std::atomic<int> fetches {0};
	for (uint64_t i = 0; i < 3; i++) {
		cache.GetBlock("id|rev", i, 10, kFile.size(), Counting(fetches));
	}
	REQUIRE(cache.BytesCached() <= 20);
	cache.GetBlock("id|rev", 2, 10, kFile.size(), Counting(fetches)); // most recent: still cached
	REQUIRE(fetches == 3);
	cache.GetBlock("id|rev", 0, 10, kFile.size(), Counting(fetches)); // oldest: evicted
	REQUIRE(fetches == 4);
}

TEST_CASE("GDriveBlockCache: a fetch that outlives Clear() accounts only to itself", "[blockcache][concurrency]") {
	// Thread A's fetch is in flight when the cache is cleared; thread B then
	// re-creates the same key and completes first. When A finishes it must not
	// add its bytes to B's entry (double counting), nor -- on failure -- erase
	// B's entry. Each entry carries the generation of its own insertion.
	GDriveBlockCache cache;
	cache.SetCapacity(1024);
	std::mutex m;
	std::condition_variable cv;
	bool a_started = false, release_a = false;
	auto blocking = [&](uint64_t start, uint64_t len, std::string &out) {
		std::unique_lock<std::mutex> lk(m);
		a_started = true;
		cv.notify_all();
		cv.wait(lk, [&] { return release_a; });
		out = kFile.substr(start, len);
	};
	std::thread a([&] { cache.GetBlock("id|rev", 0, 10, kFile.size(), blocking); });
	{
		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [&] { return a_started; });
	}
	cache.Clear();
	std::atomic<int> fetches {0};
	cache.GetBlock("id|rev", 0, 10, kFile.size(), Counting(fetches)); // B
	REQUIRE(cache.BytesCached() == 10);
	{
		std::lock_guard<std::mutex> lk(m);
		release_a = true;
	}
	cv.notify_all();
	a.join();
	REQUIRE(cache.BytesCached() == 10);
}
