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

bool GDrivePathCache::TryGet(const CacheKey &key, DriveFileMeta &out) {
	lock_guard<mutex> guard(lock);
	auto it = entries.find(key.ToString());
	if (it == entries.end()) {
		IncrementGlobalCacheMiss();
		return false;
	}
	out = it->second;
	IncrementGlobalCacheHit();
	return true;
}

void GDrivePathCache::Put(const CacheKey &key, const DriveFileMeta &meta) {
	lock_guard<mutex> guard(lock);
	entries[key.ToString()] = meta;
}

void GDrivePathCache::InvalidatePrefix(const CacheKey &key) {
	lock_guard<mutex> guard(lock);
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
	std::string prefix = secret_name;
	prefix += '\x1f';
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
}

idx_t GDrivePathCache::Size() {
	lock_guard<mutex> guard(lock);
	return entries.size();
}

} // namespace gdrive
} // namespace duckdb
