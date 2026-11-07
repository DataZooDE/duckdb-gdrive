// gdrive_versions(path): a Drive file's stored revisions, newest first. Reading
// one is the filesystem's "?version=<revision id>" path form (gdrive_uri.hpp,
// gdrive_filesystem.cpp); this lists what exists. The sibling
// duckdb-sharepoint's sharepoint_versions, over revisions.list.
#include "gdrive_versions.hpp"
#include "gdrive_banner.hpp"
#include "gdrive_filesystem.hpp"
#include "gdrive_glob.hpp"
#include "gdrive_internal.hpp"
#include "gdrive_rows.hpp"
#include "gdrive_table_util.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <algorithm>
#include <cstdint>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace gdrive {

namespace {

unique_ptr<FunctionData> VersionsBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		const auto path = PathArgument(input.inputs[0], "gdrive_versions");
		auto parsed = ParseGDriveUri(path);
		if (!parsed.ok) {
			throw InvalidInputException("gdrive: %s", parsed.error);
		}
		if (HasGlobMetacharacters(path) && parsed.uri.kind != GDriveUriKind::FILE_ID && parsed.uri.version.empty()) {
			throw InvalidInputException("gdrive: gdrive_versions takes one file, not a glob: '%s'", path);
		}
		auto opener = ClientData::Get(context).file_opener.get();
		auto &fs = GDriveFileSystem::ForDatabase(DatabaseInstance::GetDatabase(context));
		// rich: the row's revision comes from a files.get made now. The path
		// cache's headRevisionId can predate an in-place write made in this
		// process, which marked the wrong revision current (found by
		// exploratory testing after COPY ... (USE_TMP_FILE false)).
		auto items = fs.ResolveItems(path, opener, /*rich=*/true, /*folder_as_item=*/true);
		if (items.size() != 1) {
			throw InvalidInputException("gdrive: no such file: '%s'", path);
		}
		if (items[0].meta.IsFolder()) {
			throw InvalidInputException("gdrive: '%s' is a folder; gdrive_versions takes a file", path);
		}
		const auto &item = items[0];
		const auto &head = item.row.version;
		auto client = CreateGDriveClient(GetAuthContext(opener, path));
		const std::string what = "the versions of '" + path + "'";
		std::vector<RevisionRow> revisions;
		std::string token;
		for (int pages = 0;; pages++) {
			if (pages >= kMaxPages) {
				throw IOException("gdrive: reading %s exceeded %d pages", what, kMaxPages);
			}
			std::string request = "/drive/v3/files/" + QueryEncode(item.meta.id) + "/revisions?pageSize=1000&fields=" +
			                      QueryEncode("nextPageToken,revisions(" + RevisionFields() + ")") + "&" +
			                      AllDrivesQuery();
			if (!token.empty()) {
				request += "&pageToken=" + QueryEncode(token);
			}
			auto resp = client->ApiGet(request, &DriveCallStats::revisions_list);
			if (!resp.ok) {
				ThrowGDriveError(resp.error, path);
			}
			std::string error;
			if (!ParseRevisions(resp.body, revisions, token, error)) {
				throw IOException("gdrive: unexpected response reading %s: %s", what, error);
			}
			if (token.empty()) {
				break;
			}
		}
		// Drive lists oldest first; newest first is what a history view wants,
		// and what the sibling extension returns.
		std::reverse(revisions.begin(), revisions.end());

		auto data = make_uniq<RowsBindData>();
		for (size_t i = 0; i < revisions.size(); i++) {
			const auto &r = revisions[i];
			// The head revision is the file itself. A native Google file has no
			// headRevisionId, so there the newest revision is the current one.
			const bool current = head.empty() ? i == 0 : r.id == head;
			data->rows.push_back({Value(r.id),
			                      Value(current ? item.path : item.path + GDRIVE_VERSION_QUERY + r.id),
			                      Value::BOOLEAN(current), TimestampOrNull(r.modified), TextOrNull(r.modified_by.name),
			                      TextOrNull(r.modified_by.email), SizeOrNull(r.size), TextOrNull(r.md5),
			                      Value::BOOLEAN(r.keep_forever)});
		}
		names = {"version", "path", "current", "modified", "modified_by", "modified_by_email", "size", "md5",
		         "keep_forever"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN,
		                LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::BOOLEAN};
		result = std::move(data);
	});
	return result;
}

} // namespace

void RegisterVersionFunctions(ExtensionLoader &loader) {
	TableFunction fn("gdrive_versions", {LogicalType::VARCHAR}, RowsScan, VersionsBind, RowsCursorInit);
	duckdb_compat::NameParameters(fn, {"path"});
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "A Drive file's stored revisions, newest first: the revision id, a path that reads that version "
	    "(gdrive://...?version=<id>), whether it is the current one, when and by whom it was saved, its size and "
	    "md5, and whether Drive keeps it forever.";
	desc.examples = {"SELECT * FROM gdrive_versions('gdrive://Finance/actuals.csv')",
	                 "SELECT * FROM read_csv('gdrive://Finance/actuals.csv?version=<revision id>')"};
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace gdrive
} // namespace duckdb
