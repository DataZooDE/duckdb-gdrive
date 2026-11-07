#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Is a file under a folder, and at what path? PURE (the lookups go through a
// callback, so there is no I/O here and nothing to mock).
//
// gdrive_changes needs this because Drive's changes feed is per DRIVE, not
// per folder: changes.list reports every file the identity can see, and says
// of each only its name and parent ids. A file is under the folder when one
// of its ancestors is the folder, and its path is the names along the way.
//
// Each folder is looked up at most once (memoised), so a feed of N changes in
// a few folders costs a few lookups, not N.
// ---------------------------------------------------------------------------

struct ChangeScopeNode {
	std::string name;
	std::vector<std::string> parents;
};

class ChangeScope {
public:
	//! `lookup(id, node)` fills a folder's name and parents and returns
	//! false when the folder cannot be seen (gone, or not shared with this
	//! identity) -- which places everything below it out of scope. It may
	//! throw; that propagates.
	using Lookup = std::function<bool(const std::string &id, ChangeScopeNode &node)>;

	ChangeScope(std::string folder_id, Lookup lookup);

	//! Whether an item with this name and these parents is (strictly) under
	//! the folder; when it is, `relative_path` is its path below the folder
	//! ("a/b/c.csv"). Several parents (a legacy multi-parent file): the first
	//! one that leads to the folder wins.
	bool Locate(const std::string &name, const std::vector<std::string> &parents, std::string &relative_path);

	//! Lookups made so far: the cost of the walk, for tests and diagnostics.
	size_t Lookups() const {
		return lookups;
	}

private:
	struct Entry {
		bool in = false;
		//! Path of this folder below the scope folder; empty for the folder itself.
		std::string relative_path;
	};

	//! Where folder `id` stands; memoised. `depth` guards against a parent
	//! cycle, which Drive should never produce but a bug elsewhere could.
	const Entry &Folder(const std::string &id, int depth);

	std::string folder_id;
	Lookup lookup;
	std::unordered_map<std::string, Entry> memo;
	size_t lookups = 0;
};

} // namespace gdrive
} // namespace duckdb
