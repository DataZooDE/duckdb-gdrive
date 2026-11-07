// gdrive_permissions(path): who can reach a file or folder -- users, groups,
// domains and "anyone with the link" -- one row per permission. A file or
// folder by its path, or the files a glob matches. The sibling
// duckdb-sharepoint's sharepoint_permissions, over permissions.list.
// The parsing is pure (gdrive_rows.*); the listing is here.
#include "gdrive_permissions_functions.hpp"
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

unique_ptr<FunctionData> PermissionsBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		const auto path = PathArgument(input.inputs[0], "gdrive_permissions");
		auto opener = ClientData::Get(context).file_opener.get();
		auto &fs = GDriveFileSystem::ForDatabase(DatabaseInstance::GetDatabase(context));
		// Rows for web_url (a link permission's address): free for listed
		// matches, one files.get for a literal path.
		auto items = fs.ResolveItems(path, opener, /*rich=*/true, /*folder_as_item=*/true);
		auto data = make_uniq<RowsBindData>();
		if (!items.empty()) {
			auto client = CreateGDriveClient(GetAuthContext(opener, path));
			for (auto &item : items) {
				const std::string what = "the permissions of '" + item.path + "'";
				std::string token;
				for (int pages = 0;; pages++) {
					if (pages >= kMaxPages) {
						throw IOException("gdrive: reading %s exceeded %d pages", what, kMaxPages);
					}
					std::string request = "/drive/v3/files/" + QueryEncode(item.meta.id) +
					                      "/permissions?pageSize=100&fields=" +
					                      QueryEncode("nextPageToken,permissions(" + PermissionFields() + ")") + "&" +
					                      AllDrivesQuery();
					if (!token.empty()) {
						request += "&pageToken=" + QueryEncode(token);
					}
					auto resp = client->ApiGet(request, &DriveCallStats::permissions_list);
					if (!resp.ok) {
						ThrowGDriveError(resp.error, item.path);
					}
					std::vector<PermissionRow> rows;
					std::string error;
					if (!ParsePermissions(resp.body, rows, token, error)) {
						throw IOException("gdrive: unexpected response reading %s: %s", what, error);
					}
					for (auto &r : rows) {
						data->rows.push_back({Value(item.path), Value(item.meta.id), TextOrNull(r.permission_id),
						                      TextOrNull(r.role), Value(r.granted_by), TextOrNull(r.grantee_type),
						                      TextOrNull(r.grantee), TextOrNull(r.grantee_email),
						                      TextOrNull(r.link_scope),
						                      // Anyone in the scope reaches the file by its own address.
						                      r.link_scope.empty() ? Value(LogicalType::VARCHAR)
						                                           : TextOrNull(item.row.web_url),
						                      r.has_discoverable ? Value::BOOLEAN(r.discoverable)
						                                         : Value(LogicalType::BOOLEAN),
						                      TextOrNull(r.inherited_from), TimestampOrNull(r.expires)});
					}
					if (token.empty()) {
						break;
					}
				}
			}
		}
		names = {"path",          "file_id",    "permission_id", "role",         "granted_by",     "grantee_type",
		         "grantee",       "grantee_email", "link_scope", "link_url",     "discoverable",   "inherited_from",
		         "expires"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR,
		                LogicalType::TIMESTAMP};
		result = std::move(data);
	});
	return result;
}

} // namespace

void RegisterPermissionFunctions(ExtensionLoader &loader) {
	TableFunction fn("gdrive_permissions", {LogicalType::VARCHAR}, RowsScan, PermissionsBind, RowsCursorInit);
	duckdb_compat::NameParameters(fn, {"path"});
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "Who can reach a Drive file or folder (or the files a glob matches): one row per permission -- role, "
	    "granted_by (direct, inherited, link), the user, group or domain, and for general access its link_scope "
	    "(anonymous = anyone with the link, organization = a domain), link_url, and whether it is discoverable in "
	    "search.";
	desc.examples = {"SELECT path, grantee, role, link_scope FROM gdrive_permissions('gdrive://Finance/**')",
	                 "SELECT path FROM gdrive_permissions('gdrive://Finance/**') WHERE link_scope = 'anonymous'"};
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace gdrive
} // namespace duckdb
