// S-2.6/S-2.7/S-2.8/S-2.11 -- the resolver cache (HLD section 4, R-1).
//
// See the SECURITY note on CacheKey in gdrive_filesystem.hpp: the filesystem
// is registered once and spans every ClientContext, so a cache entry is keyed
// by identity (secret name, drive id, root folder) AND path, never by path
// alone. Because the key itself encodes identity, a lookup can only ever hit
// an entry stored under the same identity -- there is no separate "does this
// match the current context" check to forget.
#include "gdrive_filesystem.hpp"
#include "gdrive_stats.hpp"

#include <chrono>
#include <cstdint>
#include <iterator>

namespace duckdb {
namespace gdrive {

bool CacheKey::operator==(const CacheKey &other) const {
	return secret_name == other.secret_name && drive_id == other.drive_id && root_folder_id == other.root_folder_id &&
	       canonical_path == other.canonical_path;
}

std::string CacheKey::ToString() const {
	// '\x1f' (unit separator) delimits fields that could themselves contain
	// '/' or other path-ish characters, so two different (secret, path)
	// combinations can never collide onto the same string key.
	std::string out;
	out.reserve(secret_name.size() + drive_id.size() + root_folder_id.size() + canonical_path.size() + 4);
	out += secret_name;
	out += '\x1f';
	out += drive_id;
	out += '\x1f';
	out += root_folder_id;
	out += '\x1f';
	out += canonical_path;
	return out;
}

void GDrivePathCache::SetMaxEntries(idx_t max_entries_p) {
	lock_guard<mutex> guard(lock);
	max_entries = max_entries_p;
	EvictPathsLocked();
}

//! Least-recently-used eviction of path->id entries. Caller holds `lock`.
//!
//! A path mapping is cheap to rebuild -- one files.list per segment -- so
//! evicting the coldest is always safe, unlike the block cache where an
//! in-flight entry must survive.
void GDrivePathCache::EvictPathsLocked() {
	if (max_entries == 0) {
		SetGlobalPathCacheEntries(entries.size());
		return; // unbounded, by configuration
	}
	while (max_entries != 0 && entries.size() > max_entries) {
		auto victim = entries.begin();
		for (auto it = entries.begin(); it != entries.end(); ++it) {
			if (it->second.used_at < victim->second.used_at) {
				victim = it;
			}
		}
		entries.erase(victim);
	}
	SetGlobalPathCacheEntries(entries.size());
}

bool GDrivePathCache::TryGet(const CacheKey &key, DriveFileMeta &out) {
	lock_guard<mutex> guard(lock);
	auto it = entries.find(key.ToString());
	if (it == entries.end()) {
		IncrementGlobalCacheMiss();
		return false;
	}
	it->second.used_at = ++path_clock;
	out = it->second.meta;
	IncrementGlobalCacheHit();
	return true;
}

void GDrivePathCache::Put(const CacheKey &key, const DriveFileMeta &meta) {
	lock_guard<mutex> guard(lock);
	entries[key.ToString()] = PathEntry {meta, ++path_clock};
	EvictPathsLocked();
	SetGlobalPathCacheEntries(entries.size());
}

namespace {

//! Identity + file id. NOT file id alone -- see the header's SECURITY note.
std::string MetadataKey(const CacheKey &identity, const std::string &file_id) {
	std::string out;
	out += identity.secret_name;
	out += '\x1f';
	out += identity.drive_id;
	out += '\x1f';
	out += identity.root_folder_id;
	out += '\x1f';
	// A literal that cannot appear in an identity field or a Drive file id,
	// so a metadata key can never collide with anything else stored here.
	out += "meta";
	out += '\x1f';
	out += file_id;
	return out;
}

} // namespace

void GDrivePathCache::BeginQuery(idx_t generation) {
	lock_guard<mutex> guard(lock);
	if (generation != metadata_generation) {
		// A new query revalidates. Everything cached for the previous one is
		// discarded, so metadata is never served across a query boundary.
		metadata_entries.clear();
		metadata_generation = generation;
	}
}

bool GDrivePathCache::GetOrFetchMetadata(const CacheKey &identity, const std::string &file_id,
                                          const std::function<bool(DriveFileMeta &)> &fetch, DriveFileMeta &out) {
	const std::string key = MetadataKey(identity, file_id);

	std::shared_future<shared_ptr<const DriveFileMeta>> future;
	std::promise<shared_ptr<const DriveFileMeta>> promise;
	bool i_fetch = false;

	{
		lock_guard<mutex> guard(lock);
		if (metadata_generation == 0) {
			// No query context (see BeginQuery). Do not cache and do not
			// serve: an entry we cannot scope is one we cannot safely reuse.
			DriveFileMeta fresh;
			// Fetch outside the lock -- fall through below.
			i_fetch = false;
			(void)fresh;
		} else {
			auto it = metadata_entries.find(key);
			if (it != metadata_entries.end()) {
				future = it->second.value;
				IncrementGlobalCacheHit();
			} else {
				// Publish the future BEFORE releasing the lock, so every other
				// thread that wants this file waits on our fetch instead of
				// starting its own.
				future = promise.get_future().share();
				metadata_entries.emplace(key, MetaEntry {future});
				i_fetch = true;
				IncrementGlobalCacheMiss();
			}
		}
	}

	if (!i_fetch && !future.valid()) {
		// Uncacheable (generation 0): straight through, every time.
		return fetch(out);
	}

	if (i_fetch) {
		try {
			DriveFileMeta fresh;
			if (!fetch(fresh)) {
				// Absent. Do NOT cache: a file created a moment later must be
				// visible, and this is not the hot path.
				{
					lock_guard<mutex> guard(lock);
					metadata_entries.erase(key);
				}
				promise.set_value(nullptr);
				return false;
			}
			auto stored = make_shared_ptr<const DriveFileMeta>(fresh);
			promise.set_value(stored);
			out = fresh;
			return true;
		} catch (...) {
			{
				lock_guard<mutex> guard(lock);
				metadata_entries.erase(key);
			}
			promise.set_exception(std::current_exception());
			throw;
		}
	}

	auto got = future.get();
	if (!got) {
		return false;
	}
	out = *got;
	return true;
}

void GDrivePathCache::InvalidatePrefix(const CacheKey &key) {
	lock_guard<mutex> guard(lock);
	// Metadata is keyed by file ID, not by path, so a path-prefix walk cannot
	// find the entries this invalidation should kill -- we do not know which
	// ids lived under that path. Drop ALL metadata for this identity instead.
	//
	// Coarse on purpose. The alternative is tracking a path->id->metadata
	// chain and getting it exactly right on every rename, overwrite and
	// delete; the cost of being wrong is serving a stale size for a file that
	// was just rewritten, and the cost of being coarse is a few extra
	// files.get calls after a write. That is not a close trade.
	std::string identity = key.secret_name;
	identity += '\x1f';
	identity += key.drive_id;
	identity += '\x1f';
	identity += key.root_folder_id;
	identity += '\x1f';
	identity += "meta";
	identity += '\x1f';
	for (auto it = metadata_entries.begin(); it != metadata_entries.end();) {
		it = (it->first.compare(0, identity.size(), identity) == 0) ? metadata_entries.erase(it) : std::next(it);
	}
	// Everything under the same identity whose canonical_path is `key`'s path
	// or a descendant of it (path + "/" + anything) must go: a rename or
	// delete invalidates the whole subtree, not just the one entry.
	std::string identity_prefix = key.secret_name;
	identity_prefix += '\x1f';
	identity_prefix += key.drive_id;
	identity_prefix += '\x1f';
	identity_prefix += key.root_folder_id;
	identity_prefix += '\x1f';

	std::string exact = identity_prefix + key.canonical_path;
	std::string dir_prefix = exact + "/";

	for (auto it = entries.begin(); it != entries.end();) {
		const std::string &k = it->first;
		bool same_identity = k.compare(0, identity_prefix.size(), identity_prefix) == 0;
		if (same_identity && (k == exact || k.compare(0, dir_prefix.size(), dir_prefix) == 0)) {
			it = entries.erase(it);
		} else {
			++it;
		}
	}
}

void GDrivePathCache::InvalidateSecret(const std::string &secret_name) {
	lock_guard<mutex> guard(lock);
	// Both maps are keyed with the secret name first, so one prefix serves
	// both. Metadata entries go too: they describe files reachable only
	// through this secret's identity.
	std::string prefix = secret_name;
	prefix += '\x1f';
	for (auto it = metadata_entries.begin(); it != metadata_entries.end();) {
		it = (it->first.compare(0, prefix.size(), prefix) == 0) ? metadata_entries.erase(it) : std::next(it);
	}
	for (auto it = entries.begin(); it != entries.end();) {
		if (it->first.compare(0, prefix.size(), prefix) == 0) {
			it = entries.erase(it);
		} else {
			++it;
		}
	}
}

void GDrivePathCache::Clear() {
	lock_guard<mutex> guard(lock);
	entries.clear();
	metadata_entries.clear();
}

idx_t GDrivePathCache::Size() {
	lock_guard<mutex> guard(lock);
	return entries.size();
}

// ---------------------------------------------------------------------------
// GDriveBlockCache -- see the contract in gdrive_filesystem.hpp for WHY this
// is shared rather than per-handle, and why blocks are large.
// ---------------------------------------------------------------------------

void GDriveBlockCache::SetCapacity(idx_t bytes) {
	lock_guard<mutex> guard(lock);
	capacity_bytes = bytes;
	EvictLocked();
}

idx_t GDriveBlockCache::BytesCached() {
	lock_guard<mutex> guard(lock);
	return cached_bytes;
}

void GDriveBlockCache::Clear() {
	lock_guard<mutex> guard(lock);
	blocks.clear();
	cached_bytes = 0;
}

void GDriveBlockCache::InvalidateSecret(const std::string &secret_name) {
	lock_guard<mutex> guard(lock);
	std::string prefix = secret_name;
	prefix += '\x1f';
	for (auto it = blocks.begin(); it != blocks.end();) {
		if (it->first.compare(0, prefix.size(), prefix) == 0) {
			cached_bytes -= it->second.bytes;
			it = blocks.erase(it);
		} else {
			++it;
		}
	}
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

shared_ptr<const std::string> GDriveBlockCache::GetBlock(const std::string &key, idx_t block_index, idx_t block_size,
                                                          idx_t file_size,
                                                          const std::function<void(idx_t, idx_t, std::string &)> &fetch) {
	std::string full_key = key;
	full_key += '\x1f';
	full_key += std::to_string(block_index);

	std::shared_future<shared_ptr<const std::string>> future;
	std::promise<shared_ptr<const std::string>> promise;
	bool i_fetch = false;

	{
		lock_guard<mutex> guard(lock);
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
			entry.bytes = 0;
			entry.used_at = ++clock;
			blocks.emplace(full_key, entry);
			i_fetch = true;
		}
	}

	if (i_fetch) {
		idx_t start = block_index * block_size;
		idx_t len = MinValue<idx_t>(block_size, file_size > start ? file_size - start : 0);
		try {
			auto data = make_shared_ptr<std::string>();
			fetch(start, len, *data);
			auto stored = shared_ptr<const std::string>(std::move(data));
			{
				lock_guard<mutex> guard(lock);
				auto it = blocks.find(full_key);
				if (it != blocks.end()) {
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
				lock_guard<mutex> guard(lock);
				auto it = blocks.find(full_key);
				if (it != blocks.end()) {
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
