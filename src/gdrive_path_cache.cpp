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

bool GDrivePathCache::TryGetMetadata(const CacheKey &identity, const std::string &file_id, DriveFileMeta &out) {
	lock_guard<mutex> guard(lock);
	if (metadata_generation == 0) {
		// No query context (see BeginQuery's caller). Never serve a cached
		// entry we cannot scope -- fetching again is cheap next to being
		// wrong.
		IncrementGlobalCacheMiss();
		return false;
	}
	auto it = metadata_entries.find(MetadataKey(identity, file_id));
	if (it == metadata_entries.end()) {
		IncrementGlobalCacheMiss();
		return false;
	}
	out = it->second;
	IncrementGlobalCacheHit();
	return true;
}

void GDrivePathCache::PutMetadata(const CacheKey &identity, const std::string &file_id, const DriveFileMeta &meta) {
	lock_guard<mutex> guard(lock);
	if (metadata_generation == 0) {
		return;
	}
	metadata_entries[MetadataKey(identity, file_id)] = meta;
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

} // namespace gdrive
} // namespace duckdb
