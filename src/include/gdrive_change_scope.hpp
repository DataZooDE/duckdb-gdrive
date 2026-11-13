#pragma once

#include "gdrive_rows.hpp"

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
	//! one that leads to the folder wins. `representable`, when given, is set
	//! false if a name along the way contains '/': then no joined path reads
	//! back, and the caller must address the item by id.
	bool Locate(const std::string &name, const std::vector<std::string> &parents, std::string &relative_path,
	            bool *representable = nullptr);

	//! Lookups made so far: the cost of the walk, for tests and diagnostics.
	size_t Lookups() const {
		return lookups;
	}

private:
	struct Entry {
		bool in = false;
		//! Path of this folder below the scope folder; empty for the folder itself.
		std::string relative_path;
		//! No name on the way down contains '/'.
		bool representable = true;
	};

	//! Where folder `id` stands; memoised. `depth` guards against a parent
	//! cycle, which Drive should never produce but a bug elsewhere could.
	const Entry &Folder(const std::string &id, int depth);

	std::string folder_id;
	Lookup lookup;
	std::unordered_map<std::string, Entry> memo;
	size_t lookups = 0;
};

// ---------------------------------------------------------------------------
// Confirm before delete (2026-11-13). PURE.
//
// A change entry that cannot be placed -- `removed`, no file record, or a file
// with no parents -- used to become a path-less 'delete' (or nothing) on the
// feed's word alone. Drive's feed is read from replicas that lag each other,
// so a LIVE file of the folder can arrive in exactly that shape, and a mirror
// that drops it never sees it again once it has moved past the token. So such
// an entry is checked with ONE files.get first, and the answer decides.
// Entries with parents are placed as before, at no extra cost.
// ---------------------------------------------------------------------------

//! What a change entry says about its file, in the order the checks apply.
enum class ChangeEntryKind {
	REMOVED,    //!< removed: true -- Drive says only the id
	NO_FILE,    //!< not removed, yet no file record: treated like REMOVED
	NO_PARENTS, //!< a file record without parents: nothing to place it by
	TRASHED,    //!< in the trash, with parents: placed as before
	PLACEABLE,  //!< alive, with parents: placed as before
};

ChangeEntryKind ClassifyChangeEntry(bool removed, bool has_file, bool has_parents, bool trashed);

//! What the confirming files.get found.
struct ChangeLookup {
	bool found = false;   //!< false: 404 -- gone, or not visible to this identity
	bool trashed = false; //!< found, and in the trash
	bool located = false; //!< found, and ChangeScope::Locate places it under the folder
};

enum class ChangeVerdict {
	KEEP_AS_IS,         //!< the entry has parents: today's placement, no lookup
	LOOK_UP,            //!< confirm with files.get, then decide again with the answer
	UPSERT_FROM_LOOKUP, //!< alive under the folder: an upsert built from files.get
	DELETE_AT_PATH,     //!< trashed under the folder: a delete with its path
	DELETE,             //!< a path-less delete by id
	NOTHING,            //!< no row
};

//! The whole decision. Without `lookup`, says whether one is needed
//! (LOOK_UP) or the entry keeps today's placement (KEEP_AS_IS); with it, the
//! row to emit. `outside` is gdrive_changes' `outside := true`.
ChangeVerdict DecideChange(ChangeEntryKind kind, const ChangeLookup *lookup, bool outside);

enum class ConfirmReply {
	FOUND, //!< 200, and `file` is filled
	GONE,  //!< 404: deleted, or not visible to this identity
	FAILED, //!< anything else; `error` says why when the body was the problem
};

//! Reads the confirming files.get's answer: status and body as Drive sent them.
ConfirmReply InterpretConfirmReply(int http_status, const std::string &body, FileRow &file, std::string &error);

} // namespace gdrive
} // namespace duckdb
