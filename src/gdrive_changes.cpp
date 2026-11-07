// gdrive_changes(folder, token := ...): what changed under a folder since a
// token. The sibling duckdb-sharepoint's sharepoint_changes, over Drive's
// changes feed.
//
// Drive's feed is per DRIVE, not per folder (changes.list reports every file
// the identity can see), so each change is placed against the folder by
// walking its parents (ChangeScope, pure), one files.get per folder at most.
// A change the walk puts outside the folder is reported as a 'delete' by id:
// the file may have been moved OUT, and a mirror must drop it -- deleting an
// id the mirror never had is harmless, missing a move-out is not.
#include "gdrive_changes.hpp"
#include "gdrive_banner.hpp"
#include "gdrive_change_scope.hpp"
#include "gdrive_filesystem.hpp"
#include "gdrive_glob.hpp"
#include "gdrive_internal.hpp"
#include "gdrive_rows.hpp"
#include "gdrive_table_util.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace gdrive {

namespace {

enum class ChangeKind { UPSERT, REMOVE, CHECKPOINT };

const char *KindName(ChangeKind kind) {
	switch (kind) {
	case ChangeKind::UPSERT:
		return "upsert";
	case ChangeKind::REMOVE:
		return "delete";
	default:
		return "checkpoint";
	}
}

//! One output row: _change, id, path, name, is_folder, mime_type, size,
//! modified, modified_by, modified_by_email, token.
vector<Value> ChangeValues(ChangeKind kind, const std::string &id, const std::string &path, const FileRow *file,
                           const std::string &token) {
	const bool with_file = file && kind == ChangeKind::UPSERT;
	return {Value(KindName(kind)),
	        TextOrNull(id),
	        TextOrNull(path),
	        with_file ? TextOrNull(file->name) : Value(LogicalType::VARCHAR),
	        with_file ? Value::BOOLEAN(file->IsFolder()) : Value(LogicalType::BOOLEAN),
	        with_file ? TextOrNull(file->mime_type) : Value(LogicalType::VARCHAR),
	        with_file ? SizeOrNull(file->size) : Value(LogicalType::BIGINT),
	        with_file ? TimestampOrNull(file->modified) : Value(LogicalType::TIMESTAMP),
	        with_file ? TextOrNull(file->modified_by.name) : Value(LogicalType::VARCHAR),
	        with_file ? TextOrNull(file->modified_by.email) : Value(LogicalType::VARCHAR),
	        TextOrNull(token)};
}

//! "?driveId=..." for a secret bound to a Shared Drive: its changes feed is
//! separate from the user's.
std::string DriveScope(const GDriveAuthContext &auth) {
	return auth.drive_id.empty() ? std::string() : "&driveId=" + QueryEncode(auth.drive_id);
}

std::string StartPageToken(GDriveClient &client, const GDriveAuthContext &auth, const std::string &what) {
	auto resp =
	    client.ApiGet("/drive/v3/changes/startPageToken?" + AllDrivesQuery() + DriveScope(auth), &DriveCallStats::changes_list);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, what);
	}
	std::string token, error;
	if (!ParseStartPageToken(resp.body, token, error)) {
		throw IOException("gdrive: unexpected response reading %s: %s", what, error);
	}
	return token;
}

//! The rows of one call, at most one per id: a folder event expands to its
//! subtree, and a later change of the same item (or an explicit change of a
//! child the expansion already reported) must replace the earlier row, not
//! add a second one -- the mirror recipe INSERTs every upsert row.
struct ChangeRows {
	vector<vector<Value>> rows;
	std::unordered_map<std::string, size_t> index;

	void Put(const std::string &id, vector<Value> row) {
		auto hit = index.find(id);
		if (hit == index.end()) {
			index.emplace(id, rows.size());
			rows.push_back(std::move(row));
		} else {
			rows[hit->second] = std::move(row);
		}
	}
};

//! A path that reads back: names joined under the folder, except where that
//! would be ambiguous -- a name shared by siblings, a '/' inside a name, or an
//! id: folder with no path to join onto. Then the item's own id.
std::string ChangePath(const std::string &folder_path, bool by_id, const std::string &relative,
                       const std::string &id) {
	return by_id ? std::string(GDRIVE_SCHEME) + "id:" + id : JoinGDrivePath(folder_path, relative);
}

unique_ptr<FunctionData> ChangesBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		const auto path = PathArgument(input.inputs[0], "gdrive_changes");
		bool have_token = false;
		std::string token;
		auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "token");
		if (v && !v->IsNull()) {
			token = StringValue::Get(*v);
			have_token = true;
			if (token.empty()) {
				throw InvalidInputException("gdrive_changes: token must not be empty (leave it out to list "
				                            "everything, or pass 'latest')");
			}
		}
		// outside := true also reports what the walk places OUTSIDE the folder,
		// as path-less deletes: a file or folder moved out looks exactly like
		// one that was never in, and a mirror must drop it. Off by default --
		// Drive's feed is per drive, so it reports every change the identity
		// can see, each costing a parent walk.
		bool outside = false;
		auto o = duckdb_compat::FindNamedParameter(input.named_parameters, "outside");
		if (o && !o->IsNull()) {
			outside = BooleanValue::Get(*o);
		}
		auto parsed = ParseGDriveUri(path);
		if (!parsed.ok) {
			throw InvalidInputException("gdrive: %s", parsed.error);
		}
		if (parsed.uri.kind != GDriveUriKind::FILE_ID && HasGlobMetacharacters(path)) {
			throw InvalidInputException("gdrive_changes takes one folder, not a glob (its whole subtree is "
			                            "included): '%s'",
			                            path);
		}
		auto opener = ClientData::Get(context).file_opener.get();
		auto &fs = GDriveFileSystem::ForDatabase(DatabaseInstance::GetDatabase(context));
		auto items = fs.ResolveItems(path, opener, /*rich=*/false, /*folder_as_item=*/true);
		if (items.size() != 1) {
			throw InvalidInputException("gdrive: no such folder: '%s'", path);
		}
		if (!items[0].meta.IsFolder()) {
			throw InvalidInputException("gdrive: '%s' is a file; gdrive_changes takes a folder", path);
		}
		const auto folder_id = items[0].meta.id;
		const bool folder_by_id = parsed.uri.kind == GDriveUriKind::FILE_ID;
		const auto folder_path = CanonicalPathOf(parsed.uri);
		const std::string what = "the changes under '" + path + "'";

		auto auth = GetAuthContext(opener, path);
		auto client = CreateGDriveClient(auth);
		ChangeRows out;

		// Everything under folder `id` (whose path below the watched folder is
		// `relative`), as `kind` rows. Upserts carry paths; a name shared by
		// siblings, or with a '/', switches that item AND its subtree to id:
		// paths, because no joined path below it reads back.
		std::function<void(const std::string &, const std::string &, bool, ChangeKind)> walk;
		walk = [&](const std::string &id, const std::string &relative, bool by_id, ChangeKind kind) {
			std::vector<FileRow> rows;
			auto resp =
			    client->List("'" + EscapeDriveQueryLiteral(id) + "' in parents and trashed=false", "", FileRowFields());
			if (!resp.ok) {
				ThrowGDriveError(resp.error, path);
			}
			std::string next, error;
			if (!ParseFileRowList(resp.body, rows, next, error)) {
				throw IOException("gdrive: unexpected response reading %s: %s", what, error);
			}
			std::unordered_map<std::string, int> name_counts;
			for (auto &row : rows) {
				name_counts[row.name]++;
			}
			for (auto &row : rows) {
				const auto rel = relative.empty() ? row.name : relative + "/" + row.name;
				const bool child_by_id =
				    by_id || name_counts[row.name] > 1 || row.name.find('/') != std::string::npos;
				out.Put(row.id, ChangeValues(kind, row.id,
				                             kind == ChangeKind::UPSERT ? ChangePath(folder_path, child_by_id, rel, row.id)
				                                                        : std::string(),
				                             &row, ""));
				if (row.IsFolder()) {
					walk(row.id, rel, child_by_id, kind);
				}
			}
		};

		if (!have_token || token == "latest") {
			// The token FIRST, then the listing: a change made in between then
			// shows up twice (here and next time), never not at all.
			const auto start = StartPageToken(*client, auth, what);
			if (!have_token) {
				walk(folder_id, "", folder_by_id, ChangeKind::UPSERT);
			}
			out.rows.push_back(ChangeValues(ChangeKind::CHECKPOINT, "", "", nullptr, start));
		} else {
			ChangeScope scope(folder_id, [&](const std::string &id, ChangeScopeNode &node) {
				auto resp = client->ApiGet("/drive/v3/files/" + QueryEncode(id) + "?fields=" +
				                               QueryEncode("id,name,parents") + "&" + AllDrivesQuery(),
				                           &DriveCallStats::files_get);
				if (!resp.ok) {
					// Gone, or not visible to this identity: everything below
					// it is out of scope. Anything else is a real failure.
					if (resp.error.kind == GDriveErrorKind::NOT_FOUND) {
						return false;
					}
					ThrowGDriveError(resp.error, what);
				}
				FileRow folder;
				std::string error;
				if (!ParseFileRow(resp.body, folder, error)) {
					throw IOException("gdrive: unexpected response reading %s: %s", what, error);
				}
				node.name = folder.name;
				node.parents = folder.parents;
				return true;
			});
			// Siblings' names per parent folder, listed once each: a changed file
			// whose name a sibling shares has no path that reads back.
			std::unordered_map<std::string, std::unordered_map<std::string, int>> sibling_names;
			auto shares_name = [&](const std::string &parent_id, const std::string &name) {
				auto hit = sibling_names.find(parent_id);
				if (hit == sibling_names.end()) {
					auto resp = client->List("'" + EscapeDriveQueryLiteral(parent_id) + "' in parents and trashed=false");
					if (!resp.ok) {
						ThrowGDriveError(resp.error, what);
					}
					std::vector<DriveFileMeta> children;
					std::string next;
					if (!ParseFileList(resp.body, children, next)) {
						throw IOException("gdrive: unexpected response reading %s", what);
					}
					std::unordered_map<std::string, int> counts;
					for (auto &child : children) {
						counts[child.name]++;
					}
					hit = sibling_names.emplace(parent_id, std::move(counts)).first;
				}
				auto count = hit->second.find(name);
				return count != hit->second.end() && count->second > 1;
			};

			std::string page = token;
			std::string next_start;
			for (int pages = 0; !page.empty(); pages++) {
				if (pages >= kMaxPages) {
					throw IOException("gdrive: reading %s exceeded %d pages", what, kMaxPages);
				}
				auto resp = client->ApiGet("/drive/v3/changes?pageToken=" + QueryEncode(page) +
				                               "&pageSize=1000&includeRemoved=true&fields=" +
				                               QueryEncode("nextPageToken,newStartPageToken,changes(" +
				                                           ChangeFields() + ")") +
				                               "&" + AllDrivesQuery() + DriveScope(auth),
				                           &DriveCallStats::changes_list);
				if (!resp.ok) {
					// Drive answers a token it does not know (expired, or not a
					// token) with 400 "Invalid Value" on pageToken. Only then is
					// the token to blame; a stale DRIVE_ID 404s for other reasons.
					if (resp.http_status == 400 && resp.body.find("pageToken") != std::string::npos) {
						throw InvalidInputException(
						    "gdrive_changes: Drive does not accept the token '%s' -- it may have expired, or "
						    "belong to another drive. Start over without a token (or with token := 'latest').",
						    token);
					}
					ThrowGDriveError(resp.error, what);
				}
				std::vector<ChangeRow> changes;
				std::string next, error;
				if (!ParseChanges(resp.body, changes, next, next_start, error)) {
					throw IOException("gdrive: unexpected response reading %s: %s", what, error);
				}
				for (auto &c : changes) {
					// The folder itself is not under itself; adding children
					// touches it, and a listing never includes it either.
					if (c.file_id == folder_id) {
						continue;
					}
					// Removed (deleted for good, or no longer visible): Drive says
					// only the id, so it cannot be placed. Always reported -- it
					// costs nothing, and a permanently deleted file of the folder
					// must not be missed.
					if (c.removed || !c.has_file) {
						out.Put(c.file_id, ChangeValues(ChangeKind::REMOVE, c.file_id, "", nullptr, ""));
						continue;
					}
					std::string relative;
					bool representable = true;
					const bool located = scope.Locate(c.file.name, c.file.parents, relative, &representable);
					if (!located) {
						if (outside) {
							out.Put(c.file_id, ChangeValues(ChangeKind::REMOVE, c.file_id, "", nullptr, ""));
							// A folder moved out takes its subtree with it, and the
							// feed reports only the folder.
							if (c.file.IsFolder() && !c.file.trashed) {
								walk(c.file_id, "", true, ChangeKind::REMOVE);
							}
						}
						continue;
					}
					const bool by_id = folder_by_id || !representable ||
					                   (!c.file.parents.empty() && shares_name(c.file.parents[0], c.file.name));
					const auto change_path = ChangePath(folder_path, by_id, relative, c.file_id);
					if (c.file.trashed) {
						out.Put(c.file_id, ChangeValues(ChangeKind::REMOVE, c.file_id, change_path, nullptr, ""));
						continue;
					}
					out.Put(c.file_id, ChangeValues(ChangeKind::UPSERT, c.file_id, change_path, &c.file, ""));
					// A folder renamed or moved in: its subtree's paths changed
					// (or it just arrived), and the feed reports only the folder.
					if (c.file.IsFolder()) {
						walk(c.file_id, relative, by_id, ChangeKind::UPSERT);
					}
				}
				page = next;
			}
			out.rows.push_back(ChangeValues(ChangeKind::CHECKPOINT, "", "", nullptr, next_start));
		}
		auto data = make_uniq<RowsBindData>();
		data->rows = std::move(out.rows);
		names = {"_change",  "id",       "path",        "name",
		         "is_folder", "mime_type", "size",       "modified",
		         "modified_by", "modified_by_email", "token"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::BOOLEAN, LogicalType::VARCHAR,   LogicalType::BIGINT,  LogicalType::TIMESTAMP,
		                LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR};
		result = std::move(data);
	});
	return result;
}

} // namespace

void RegisterChangeFunctions(ExtensionLoader &loader) {
	TableFunction fn("gdrive_changes", {LogicalType::VARCHAR}, RowsScan, ChangesBind, RowsCursorInit);
	duckdb_compat::NameParameters(fn, {"path"});
	duckdb_compat::AddNamedParameter(fn, "token", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "outside", LogicalType::BOOLEAN);
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "What changed under a Drive folder since a token: one row per file or folder created or changed "
	    "(_change = 'upsert'; a folder renamed or moved in brings its subtree) or trashed or deleted ('delete', by "
	    "id), then one 'checkpoint' row whose token is what to pass next time. Without a token it lists everything "
	    "under the folder once; token := 'latest' returns only a checkpoint. outside := true also reports what is "
	    "no longer under the folder (moved out, with its subtree) as path-less deletes -- what a mirror needs; "
	    "Drive cannot tell a move out from a change elsewhere, so it reports those too.";
	desc.examples = {"SELECT token FROM gdrive_changes('gdrive://Finance', token := 'latest')",
	                 "SELECT _change, path FROM gdrive_changes('gdrive://Finance', token := '12345')"};
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace gdrive
} // namespace duckdb
