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
#include "gdrive_internal.hpp"
#include "gdrive_rows.hpp"
#include "gdrive_table_util.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <cstdint>
#include <functional>

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

//! A folder's path as the user wrote it, for joining children onto: "" for
//! the root, "Finance/2026" for a path, and none at all (`by_id`) for an id:
//! folder, whose children are then addressed by their own ids.
std::string JoinChild(const std::string &folder_path, bool by_id, const std::string &relative, const std::string &id) {
	if (by_id) {
		return std::string(GDRIVE_SCHEME) + "id:" + id;
	}
	return JoinGDrivePath(folder_path, relative);
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
		auto parsed = ParseGDriveUri(path);
		if (!parsed.ok) {
			throw InvalidInputException("gdrive: %s", parsed.error);
		}
		auto opener = ClientData::Get(context).file_opener.get();
		auto &fs = GDriveFileSystem::ForDatabase(DatabaseInstance::GetDatabase(context));
		auto items = fs.ResolveItems(path, opener, /*rich=*/false, /*folder_as_item=*/true);
		if (items.size() != 1 || !items[0].meta.IsFolder()) {
			throw InvalidInputException("gdrive: no such folder: '%s' (gdrive_changes takes one folder)", path);
		}
		const auto folder_id = items[0].meta.id;
		const bool by_id = parsed.uri.kind == GDriveUriKind::FILE_ID;
		const auto folder_path = CanonicalPathOf(parsed.uri);
		const std::string what = "the changes under '" + path + "'";

		auto auth = GetAuthContext(opener, path);
		auto client = CreateGDriveClient(auth);
		auto data = make_uniq<RowsBindData>();

		if (!have_token || token == "latest") {
			// The token FIRST, then the listing: a change made in between then
			// shows up twice (here and next time), never not at all.
			const auto start = StartPageToken(*client, auth, what);
			if (!have_token) {
				std::function<void(const std::string &, const std::string &)> walk;
				walk = [&](const std::string &id, const std::string &relative) {
					std::vector<FileRow> rows;
					auto resp = client->List("'" + EscapeDriveQueryLiteral(id) + "' in parents and trashed=false", "",
					                         FileRowFields());
					if (!resp.ok) {
						ThrowGDriveError(resp.error, path);
					}
					std::string next, error;
					if (!ParseFileRowList(resp.body, rows, next, error)) {
						throw IOException("gdrive: unexpected response reading %s: %s", what, error);
					}
					for (auto &row : rows) {
						const auto rel = relative.empty() ? row.name : relative + "/" + row.name;
						data->rows.push_back(
						    ChangeValues(ChangeKind::UPSERT, row.id, JoinChild(folder_path, by_id, rel, row.id), &row, ""));
						if (row.IsFolder()) {
							walk(row.id, rel);
						}
					}
				};
				walk(folder_id, "");
			}
			data->rows.push_back(ChangeValues(ChangeKind::CHECKPOINT, "", "", nullptr, start));
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
					// token) with 400 "Invalid Value" on pageToken.
					if (resp.http_status == 400 || resp.http_status == 404) {
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
					std::string relative;
					const bool located = c.has_file && scope.Locate(c.file.name, c.file.parents, relative);
					if (c.removed || !c.has_file || c.file.trashed || !located) {
						// Removed, trashed, or (now) outside the folder: a delete,
						// with the path when the file is still where we can see it.
						data->rows.push_back(ChangeValues(ChangeKind::REMOVE, c.file_id,
						                                  located ? JoinChild(folder_path, by_id, relative, c.file_id)
						                                          : std::string(),
						                                  nullptr, ""));
					} else {
						data->rows.push_back(ChangeValues(ChangeKind::UPSERT, c.file_id,
						                                  JoinChild(folder_path, by_id, relative, c.file_id), &c.file,
						                                  ""));
					}
				}
				page = next;
			}
			data->rows.push_back(ChangeValues(ChangeKind::CHECKPOINT, "", "", nullptr, next_start));
		}
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
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "What changed under a Drive folder since a token: one row per file or folder created or changed "
	    "(_change = 'upsert') or deleted, trashed or moved out ('delete', by id; path when Drive still shows it), "
	    "then one 'checkpoint' row whose token is what to pass next time. Without a token it lists everything "
	    "under the folder once; token := 'latest' returns only a checkpoint.";
	desc.examples = {"SELECT token FROM gdrive_changes('gdrive://Finance', token := 'latest')",
	                 "SELECT _change, path FROM gdrive_changes('gdrive://Finance', token := '12345')"};
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace gdrive
} // namespace duckdb
