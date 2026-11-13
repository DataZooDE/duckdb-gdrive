// Is a file under a folder, and at what path? PURE: see gdrive_change_scope.hpp.
#include "gdrive_change_scope.hpp"
#include "gdrive_errors.hpp"

#include <utility>

namespace duckdb {
namespace gdrive {

namespace {
//! Deeper than any real Drive tree; reaching it means a parent cycle.
constexpr int kMaxDepth = 256;
} // namespace

ChangeScope::ChangeScope(std::string folder_id_p, Lookup lookup_p)
    : folder_id(std::move(folder_id_p)), lookup(std::move(lookup_p)) {
	memo[folder_id] = Entry {true, "", true};
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
					entry.representable = up.representable && node.name.find('/') == std::string::npos;
					break;
				}
			}
		}
	}
	// Re-find: the recursion above may have rehashed the map.
	return memo[id] = std::move(entry);
}

bool ChangeScope::Locate(const std::string &name, const std::vector<std::string> &parents,
                         std::string &relative_path, bool *representable) {
	for (auto &parent : parents) {
		const Entry &up = Folder(parent, 0);
		if (up.in) {
			relative_path = up.relative_path.empty() ? name : up.relative_path + "/" + name;
			if (representable) {
				*representable = up.representable && name.find('/') == std::string::npos;
			}
			return true;
		}
	}
	return false;
}

ChangeEntryKind ClassifyChangeEntry(bool removed, bool has_file, bool has_parents, bool trashed) {
	if (removed) {
		return ChangeEntryKind::REMOVED;
	}
	if (!has_file) {
		return ChangeEntryKind::NO_FILE;
	}
	if (!has_parents) {
		return ChangeEntryKind::NO_PARENTS;
	}
	return trashed ? ChangeEntryKind::TRASHED : ChangeEntryKind::PLACEABLE;
}

ChangeVerdict DecideChange(ChangeEntryKind kind, const ChangeLookup *lookup, bool outside) {
	// An entry with parents is placed by its parents, as it always was: a
	// confirm per foreign change would cost a files.get for every change the
	// identity can see.
	if (kind == ChangeEntryKind::TRASHED || kind == ChangeEntryKind::PLACEABLE) {
		return ChangeVerdict::KEEP_AS_IS;
	}
	if (!lookup) {
		return ChangeVerdict::LOOK_UP;
	}
	// Drive shows it under the folder: the entry was a lagging replica's
	// view, and the file is what files.get says it is.
	if (lookup->found && lookup->located) {
		return lookup->trashed ? ChangeVerdict::DELETE_AT_PATH : ChangeVerdict::UPSERT_FROM_LOOKUP;
	}
	// Gone, or alive (or trashed) but NOT under the folder.
	//
	// A file record without parents is placed nowhere, as before: a delete
	// only when outside := true asks for what is not under the folder.
	if (kind == ChangeEntryKind::NO_PARENTS) {
		return outside ? ChangeVerdict::DELETE : ChangeVerdict::NOTHING;
	}
	// A removed entry stays a delete even when files.get finds the file alive
	// elsewhere, with or without outside := true: alive-and-outside is exactly
	// what a file moved OUT looks like, and a mirror holding the id must drop
	// it. Removed entries were always reported for any id (Drive says only the
	// id, so nobody can tell ours from foreign ones), and deleting an id the
	// mirror never had is harmless. The confirm only ever turns a delete into
	// an upsert when Drive places the file under the folder.
	return ChangeVerdict::DELETE;
}

ConfirmReply InterpretConfirmReply(int http_status, const std::string &body, FileRow &file, std::string &error) {
	if (http_status == 200) {
		return ParseFileRow(body, file, error) ? ConfirmReply::FOUND : ConfirmReply::FAILED;
	}
	// 404 for a file deleted for good and for one this identity cannot see:
	// Drive answers both the same way, and both mean "drop it".
	if (ClassifyDriveError(http_status, body).kind == GDriveErrorKind::NOT_FOUND) {
		return ConfirmReply::GONE;
	}
	return ConfirmReply::FAILED;
}

} // namespace gdrive
} // namespace duckdb
