// read_gdrive_files(path), search_gdrive(text), gdrive_drives(): what is in
// Drive, as rows. The sibling duckdb-sharepoint's read_sharepoint_files /
// search_sharepoint / sharepoint_drives, over the Drive API.
//
// read_gdrive_files resolves its argument through the filesystem (the same
// glob, the same path cache), asking the listings for the extra fields -- so
// a glob costs exactly the listings `glob()` would, and no files.get per hit.
#include "gdrive_files_functions.hpp"
#include "gdrive_banner.hpp"
#include "gdrive_filesystem.hpp"
#include "gdrive_internal.hpp"
#include "gdrive_rows.hpp"
#include "gdrive_table_util.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <cstdint>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace gdrive {

namespace {

//! Columns of read_gdrive_files and search_gdrive, in order.
void FileColumns(vector<LogicalType> &types, duckdb_compat::BindNames &names) {
	names = {"path",        "name",        "size",    "created", "modified", "modified_by", "modified_by_email",
	         "owner",       "owner_email", "mime_type", "id",    "web_url",  "version",     "md5",
	         "description", "starred",     "shared",  "parent_id", "properties"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::BIGINT,  LogicalType::TIMESTAMP,
	         LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::BOOLEAN,
	         LogicalType::BOOLEAN, LogicalType::VARCHAR,   LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)};
}

vector<Value> FileValues(const std::string &path, const FileRow &r) {
	vector<Value> keys, values;
	for (auto &kv : r.properties) {
		keys.emplace_back(kv.first);
		values.emplace_back(kv.second);
	}
	const DrivePerson owner = r.owners.empty() ? DrivePerson() : r.owners[0];
	return {Value(path),
	        Value(r.name),
	        SizeOrNull(r.size),
	        TimestampOrNull(r.created),
	        TimestampOrNull(r.modified),
	        TextOrNull(r.modified_by.name),
	        TextOrNull(r.modified_by.email),
	        TextOrNull(owner.name),
	        TextOrNull(owner.email),
	        Value(r.mime_type),
	        Value(r.id),
	        TextOrNull(r.web_url),
	        TextOrNull(r.version),
	        TextOrNull(r.md5),
	        TextOrNull(r.description),
	        Value::BOOLEAN(r.starred),
	        Value::BOOLEAN(r.shared),
	        TextOrNull(r.parents.empty() ? std::string() : r.parents[0]),
	        Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(values))};
}

// ---------------------------------------------------------------------------
// read_gdrive_files(path)
// ---------------------------------------------------------------------------

unique_ptr<FunctionData> FilesBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		const auto path = PathArgument(input.inputs[0], "read_gdrive_files");
		auto opener = ClientData::Get(context).file_opener.get();
		auto &fs = GDriveFileSystem::ForDatabase(DatabaseInstance::GetDatabase(context));
		auto data = make_uniq<RowsBindData>();
		for (auto &item : fs.ResolveItems(path, opener, /*rich=*/true, /*folder_as_item=*/false)) {
			if (item.meta.IsFolder()) {
				continue;
			}
			data->rows.push_back(FileValues(item.path, item.row));
		}
		FileColumns(return_types, names);
		result = std::move(data);
	});
	return result;
}

// ---------------------------------------------------------------------------
// search_gdrive(text, max_results := 500, mime_type := NULL)
// ---------------------------------------------------------------------------

unique_ptr<FunctionData> SearchBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("search_gdrive: the search text must not be NULL");
		}
		const auto text = StringValue::Get(input.inputs[0]);
		int64_t max_results = 500;
		std::string mime_type;
		// Through the compat shim: DuckDB 2.0 keys named parameters by its own
		// name type, not std::string.
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "max_results")) {
			if (!v->IsNull()) {
				max_results = v->GetValue<int64_t>();
				if (max_results < 1) {
					throw InvalidInputException("search_gdrive: max_results must be at least 1");
				}
			}
		}
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "mime_type")) {
			if (!v->IsNull()) {
				mime_type = StringValue::Get(*v);
			}
		}
		std::string query = "trashed=false";
		if (!text.empty()) {
			query = "fullText contains '" + EscapeDriveQueryLiteral(text) + "' and " + query;
		}
		if (!mime_type.empty()) {
			query += " and mimeType='" + EscapeDriveQueryLiteral(mime_type) + "'";
		}

		auto opener = ClientData::Get(context).file_opener.get();
		auto auth = GetAuthContext(opener, GDRIVE_SCHEME);
		auto client = CreateGDriveClient(auth);
		// A secret bound to a Shared Drive searches that drive; any other
		// searches everything the identity can see, Shared Drives included.
		const std::string corpora = auth.drive_id.empty()
		                                ? "&corpora=allDrives"
		                                : "&corpora=drive&driveId=" + QueryEncode(auth.drive_id);
		const std::string what = "the search for '" + text + "'";
		auto data = make_uniq<RowsBindData>();
		std::string token;
		for (int pages = 0; static_cast<int64_t>(data->rows.size()) < max_results; pages++) {
			if (pages >= kMaxPages) {
				throw IOException("gdrive: %s exceeded %d pages", what, kMaxPages);
			}
			const int64_t want = std::min<int64_t>(1000, max_results - static_cast<int64_t>(data->rows.size()));
			std::string request = "/drive/v3/files?q=" + QueryEncode(query) + "&pageSize=" + std::to_string(want) +
			                      "&fields=" + QueryEncode("nextPageToken,files(" + FileRowFields() + ")") + "&" +
			                      AllDrivesQuery() + corpora;
			if (!token.empty()) {
				request += "&pageToken=" + QueryEncode(token);
			}
			auto resp = client->ApiGet(request, &DriveCallStats::files_list);
			if (!resp.ok) {
				ThrowGDriveError(resp.error, what);
			}
			std::vector<FileRow> rows;
			std::string error;
			if (!ParseFileRowList(resp.body, rows, token, error)) {
				throw IOException("gdrive: unexpected response to %s: %s", what, error);
			}
			for (auto &r : rows) {
				if (static_cast<int64_t>(data->rows.size()) >= max_results) {
					break;
				}
				// A search hit has no path we could know without walking its
				// parents; its id: path reads straight back.
				data->rows.push_back(FileValues(std::string(GDRIVE_SCHEME) + "id:" + r.id, r));
			}
			if (token.empty()) {
				break;
			}
		}
		FileColumns(return_types, names);
		result = std::move(data);
	});
	return result;
}

// ---------------------------------------------------------------------------
// gdrive_drives(search := NULL)
// ---------------------------------------------------------------------------

unique_ptr<FunctionData> DrivesBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		std::string search;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "search")) {
			if (!v->IsNull()) {
				search = StringValue::Get(*v);
			}
		}
		auto opener = ClientData::Get(context).file_opener.get();
		auto auth = GetAuthContext(opener, GDRIVE_SCHEME);
		auto client = CreateGDriveClient(auth);
		auto data = make_uniq<RowsBindData>();
		std::string token;
		for (int pages = 0;; pages++) {
			if (pages >= kMaxPages) {
				throw IOException("gdrive: listing Shared Drives exceeded %d pages", kMaxPages);
			}
			std::string request =
			    "/drive/v3/drives?pageSize=100&fields=" + QueryEncode("nextPageToken,drives(" + SharedDriveFields() + ")");
			if (!search.empty()) {
				request += "&q=" + QueryEncode("name contains '" + EscapeDriveQueryLiteral(search) + "'");
			}
			if (!token.empty()) {
				request += "&pageToken=" + QueryEncode(token);
			}
			auto resp = client->ApiGet(request, &DriveCallStats::drives_list);
			if (!resp.ok) {
				ThrowGDriveError(resp.error, "the Shared Drives");
			}
			std::vector<SharedDriveRow> rows;
			std::string error;
			if (!ParseSharedDrives(resp.body, rows, token, error)) {
				throw IOException("gdrive: unexpected response listing Shared Drives: %s", error);
			}
			for (auto &r : rows) {
				data->rows.push_back(
				    {Value(r.name), Value(r.id), TimestampOrNull(r.created), Value::BOOLEAN(r.hidden)});
			}
			if (token.empty()) {
				break;
			}
		}
		names = {"name", "id", "created", "hidden"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::BOOLEAN};
		result = std::move(data);
	});
	return result;
}

void Register(ExtensionLoader &loader, TableFunction fn, const std::string &description,
              vector<string> examples) {
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace

void RegisterFileFunctions(ExtensionLoader &loader) {
	{
		TableFunction fn("read_gdrive_files", {LogicalType::VARCHAR}, RowsScan, FilesBind, RowsCursorInit);
		duckdb_compat::NameParameters(fn, {"path"});
		Register(loader, fn,
		         "Drive files as rows, with their metadata: one row per file a gdrive:// path, glob or folder names "
		         "(a folder means the files directly in it) -- path, name, size, created, modified, modified_by(_email), "
		         "owner(_email), mime_type, id, web_url, version (the revision id), md5, description, starred, shared, "
		         "parent_id, and the file's custom properties as a MAP.",
		         {"SELECT name, size, modified, modified_by FROM read_gdrive_files('gdrive://Finance/**/*.csv')",
		          "SELECT * FROM read_gdrive_files('gdrive://Finance')"});
	}
	{
		TableFunction fn("search_gdrive", {LogicalType::VARCHAR}, RowsScan, SearchBind, RowsCursorInit);
		duckdb_compat::NameParameters(fn, {"text"});
		duckdb_compat::AddNamedParameter(fn, "max_results", LogicalType::BIGINT);
		duckdb_compat::AddNamedParameter(fn, "mime_type", LogicalType::VARCHAR);
		Register(loader, fn,
		         "Drive's full-text search over names and contents, as rows with read_gdrive_files' columns. path is "
		         "a gdrive://id: path, so a hit reads straight back. Searches what the secret's identity can see (its "
		         "Shared Drive when it binds DRIVE_ID); max_results (default 500) caps the rows, mime_type filters "
		         "exactly.",
		         {"SELECT name, path, modified FROM search_gdrive('quarterly forecast')",
		          "SELECT name FROM search_gdrive('', mime_type := 'application/vnd.google-apps.spreadsheet')"});
	}
	{
		TableFunction fn("gdrive_drives", {}, RowsScan, DrivesBind, RowsCursorInit);
		duckdb_compat::AddNamedParameter(fn, "search", LogicalType::VARCHAR);
		Register(loader, fn,
		         "The Shared Drives the secret's identity is a member of: name, id (what DRIVE_ID takes), created, "
		         "hidden. search := '<text>' keeps those whose name contains it.",
		         {"SELECT name, id FROM gdrive_drives()", "SELECT id FROM gdrive_drives(search := 'Finance')"});
	}
}

} // namespace gdrive
} // namespace duckdb
