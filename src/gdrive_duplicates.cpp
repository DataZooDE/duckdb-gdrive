#include "gdrive_duplicates.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace duckdb {
namespace gdrive {

RecentCreates::RecentCreates(std::size_t max_entries_p, int64_t window_ms_p)
    : max_entries(max_entries_p == 0 ? 1 : max_entries_p), window_ms(window_ms_p) {
}

void RecentCreates::Remember(const std::string &key, const std::string &file_id, int64_t now_ms) {
	Forget(key);
	order.push_back(key);
	entries[key] = Entry {file_id, now_ms, std::prev(order.end())};
	while (entries.size() > max_entries) {
		entries.erase(order.front());
		order.pop_front();
	}
}

std::string RecentCreates::Recall(const std::string &key, int64_t now_ms) {
	auto it = entries.find(key);
	if (it == entries.end()) {
		return "";
	}
	if (now_ms - it->second.at_ms >= window_ms) {
		Forget(key);
		return "";
	}
	return it->second.file_id;
}

void RecentCreates::Forget(const std::string &key) {
	auto it = entries.find(key);
	if (it == entries.end()) {
		return;
	}
	order.erase(it->second.order);
	entries.erase(it);
}

std::size_t RecentCreates::Size() const {
	return entries.size();
}

namespace {
bool HasNoBytes(const DuplicateCopy &c) {
	// Folders and native Docs/Sheets/Slides: no md5Checksum to compare.
	return c.mime_type.rfind("application/vnd.google-apps.", 0) == 0;
}
} // namespace

DuplicateHealPlan PlanIdenticalDuplicateHeal(const std::vector<DuplicateCopy> &copies) {
	DuplicateHealPlan plan;
	if (copies.size() < 2) {
		plan.reason = "not a duplicate";
		return plan;
	}
	for (const auto &c : copies) {
		if (HasNoBytes(c) || c.md5.empty() || c.size < 0) {
			plan.reason = "a copy has no checksum to compare (a folder or native Google file)";
			return plan;
		}
		if (c.md5 != copies.front().md5 || c.size != copies.front().size) {
			plan.reason = "the copies' contents differ";
			return plan;
		}
	}
	std::vector<const DuplicateCopy *> sorted;
	for (const auto &c : copies) {
		sorted.push_back(&c);
	}
	// RFC 3339 in UTC with Drive's fixed millisecond form orders as a string.
	std::sort(sorted.begin(), sorted.end(), [](const DuplicateCopy *a, const DuplicateCopy *b) {
		if (a->created_time != b->created_time) {
			return a->created_time < b->created_time;
		}
		return a->id < b->id;
	});
	plan.healable = true;
	plan.keep_id = sorted.front()->id;
	for (size_t i = 1; i < sorted.size(); i++) {
		plan.trash_ids.push_back(sorted[i]->id);
	}
	std::sort(plan.trash_ids.begin(), plan.trash_ids.end());
	return plan;
}

} // namespace gdrive
} // namespace duckdb
