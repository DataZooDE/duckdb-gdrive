// Is a file under a folder, and at what path? PURE: see gdrive_change_scope.hpp.
#include "gdrive_change_scope.hpp"

#include <utility>

namespace duckdb {
namespace gdrive {

namespace {
//! Deeper than any real Drive tree; reaching it means a parent cycle.
constexpr int kMaxDepth = 256;
} // namespace

ChangeScope::ChangeScope(std::string folder_id_p, Lookup lookup_p)
    : folder_id(std::move(folder_id_p)), lookup(std::move(lookup_p)) {
	memo[folder_id] = Entry {true, ""};
}

const ChangeScope::Entry &ChangeScope::Folder(const std::string &id, int depth) {
	auto hit = memo.find(id);
	if (hit != memo.end()) {
		return hit->second;
	}
	// Out of scope until proven otherwise: a parent cycle then ends here
	// instead of recursing.
	memo[id] = Entry {};
	Entry entry;
	if (depth < kMaxDepth) {
		ChangeScopeNode node;
		lookups++;
		if (lookup(id, node)) {
			for (auto &parent : node.parents) {
				const Entry &up = Folder(parent, depth + 1);
				if (up.in) {
					entry.in = true;
					entry.relative_path = up.relative_path.empty() ? node.name : up.relative_path + "/" + node.name;
					break;
				}
			}
		}
	}
	// Re-find: the recursion above may have rehashed the map.
	return memo[id] = std::move(entry);
}

bool ChangeScope::Locate(const std::string &name, const std::vector<std::string> &parents,
                         std::string &relative_path) {
	for (auto &parent : parents) {
		const Entry &up = Folder(parent, 0);
		if (up.in) {
			relative_path = up.relative_path.empty() ? name : up.relative_path + "/" + name;
			return true;
		}
	}
	return false;
}

} // namespace gdrive
} // namespace duckdb
