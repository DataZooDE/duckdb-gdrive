#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Pure helpers against duplicate names (R-4). No duckdb.hpp, no I/O.
//
// Drive lets two files share a name in one folder, and this extension refuses
// such a name (R-4). A write that targets a name looks it up first
// (ListByName, a files.list `name=` query) and creates when nothing matches.
// That query is EVENTUALLY consistent: a file created seconds ago can be
// missing from its answer. A second write of the same new name moments after
// the first then creates a second file -- the heron-escurel incident of
// 2026-11-18, where two writes of skills/agent.md 3 s apart left two
// identical copies and the gateway could no longer boot.
//
// RecentCreates is the prevention: what this process itself created, so the
// next write of that name asks Drive by id (files.get, strongly consistent)
// instead of trusting the listing. PlanIdenticalDuplicateHeal is the repair
// for a pair that exists anyway: byte-identical copies lose nothing when all
// but one go to the trash.
// ---------------------------------------------------------------------------

//! Files this process created, by an opaque key (identity, root, parent id,
//! name), each remembered for `window_ms`. Bounded: past `max_entries` the
//! oldest remembered create is dropped. Not thread-safe; the filesystem
//! guards it with its own mutex.
class RecentCreates {
public:
	explicit RecentCreates(std::size_t max_entries = 4096, int64_t window_ms = 10 * 60 * 1000);

	//! `file_id` was created under `key` at `now_ms`. A later create for the
	//! same key replaces the earlier one.
	void Remember(const std::string &key, const std::string &file_id, int64_t now_ms);
	//! The id remembered for `key`, or empty when there is none or it is
	//! older than the window (and then it is dropped).
	std::string Recall(const std::string &key, int64_t now_ms);
	void Forget(const std::string &key);
	std::size_t Size() const;

private:
	struct Entry {
		std::string file_id;
		int64_t at_ms;
		std::list<std::string>::iterator order;
	};
	std::size_t max_entries;
	int64_t window_ms;
	std::unordered_map<std::string, Entry> entries;
	//! Keys, oldest Remember first.
	std::list<std::string> order;
};

//! One of several files sharing a name in one folder.
struct DuplicateCopy {
	std::string id;
	std::string mime_type;
	//! md5Checksum; empty for folders and native Google files.
	std::string md5;
	int64_t size = -1;
	//! createdTime, RFC 3339 as Drive returns it (always UTC, "Z").
	std::string created_time;
};

struct DuplicateHealPlan {
	bool healable = false;
	//! The copy that stays: the oldest by createdTime, ties broken by id, so
	//! every process that meets the same pair keeps the same one.
	std::string keep_id;
	//! The others, in id order. Trashed, never deleted (D-6).
	std::vector<std::string> trash_ids;
	//! Why not, when !healable.
	std::string reason;
};

//! Whether `copies` can be reduced to one without losing anything: at least
//! two, none a folder or native Google file, every one with an md5Checksum,
//! and all of the same size and checksum.
DuplicateHealPlan PlanIdenticalDuplicateHeal(const std::vector<DuplicateCopy> &copies);

} // namespace gdrive
} // namespace duckdb
