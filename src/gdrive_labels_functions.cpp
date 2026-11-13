// gdrive_labels(path, names := false): the Drive labels applied to a file, or
// to the files a glob or folder names -- one row per (file, label, field,
// value). Drive's answer to duckdb-sharepoint's deferred sensitivity labels.
// The parsing is pure (gdrive_labels.*); the calls are here.
//
// Per file: one files.listLabels (Drive v3, the Drive scope suffices). With
// names := true, ONE Labels API catalog call per bind as well, for the
// human-readable names -- which needs the drive.labels.readonly scope AND the
// Drive Labels API enabled in the credential's Cloud project, hence opt-in.
#include "gdrive_labels_functions.hpp"
#include "gdrive_banner.hpp"
#include "gdrive_filesystem.hpp"
#include "gdrive_internal.hpp"
#include "gdrive_labels.hpp"
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

constexpr const char *kLabelsHost = "drivelabels.googleapis.com";

//! The catalog, for names := true. Its failures get their own hint: the
//! generic INSUFFICIENT_SCOPE text is about the Drive scope, and the fix
//! here is a different scope (and an API to enable).
LabelCatalog FetchCatalog(GDriveClient &client) {
	LabelCatalog catalog;
	const std::string what = "the Drive label catalog (gdrive_labels names := true)";
	std::string token;
	for (int pages = 0;; pages++) {
		if (pages >= kMaxPages) {
			throw IOException("gdrive: reading %s exceeded %d pages", what, kMaxPages);
		}
		std::string request = "/v2/labels?view=LABEL_VIEW_FULL&publishedOnly=true&pageSize=200&fields=" +
		                      QueryEncode(LabelCatalogFields());
		if (!token.empty()) {
			request += "&pageToken=" + QueryEncode(token);
		}
		auto resp = client.ApiGet(request, &DriveCallStats::label_catalog, kLabelsHost);
		if (!resp.ok) {
			if (resp.error.kind == GDriveErrorKind::INSUFFICIENT_SCOPE) {
				throw PermissionException(
				    "gdrive: insufficient OAuth scope for %s: label NAMES come from the Drive Labels API, which "
				    "needs the scope https://www.googleapis.com/auth/drive.labels.readonly as well as the Drive "
				    "one. This is not a file-sharing problem.\n"
				    "Without names, gdrive_labels needs neither: drop names := true and the label_id, field_id "
				    "and value columns still come back.\n"
				    "For a service account, add it to the secret's DRIVE_SCOPE, space-separated:\n"
				    "  DRIVE_SCOPE 'https://www.googleapis.com/auth/drive "
				    "https://www.googleapis.com/auth/drive.labels.readonly'\n"
				    "For a user login, sign in again requesting it (an existing refresh token keeps the scopes it "
				    "was granted), e.g. gcloud auth application-default login "
				    "--scopes=openid,https://www.googleapis.com/auth/drive,"
				    "https://www.googleapis.com/auth/drive.labels.readonly",
				    what);
			}
			if (resp.error.kind == GDriveErrorKind::SERVICE_DISABLED) {
				throw PermissionException("%s\nWithout names, gdrive_labels does not use the Labels API: drop "
				                          "names := true.",
				                          FormatUserMessage(resp.error, what));
			}
			ThrowGDriveError(resp.error, what);
		}
		std::string error;
		if (!ParseLabelCatalog(resp.body, catalog, token, error)) {
			throw IOException("gdrive: unexpected response reading %s: %s", what, error);
		}
		if (token.empty()) {
			break;
		}
	}
	return catalog;
}

unique_ptr<FunctionData> LabelsBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(GDRIVE_BANNER, {
		const auto path = PathArgument(input.inputs[0], "gdrive_labels");
		bool with_names = false;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "names")) {
			if (!v->IsNull()) {
				with_names = BooleanValue::Get(*v);
			}
		}
		auto opener = ClientData::Get(context).file_opener.get();
		auto &fs = GDriveFileSystem::ForDatabase(DatabaseInstance::GetDatabase(context));
		auto client = CreateGDriveClient(GetAuthContext(opener, path));
		// The catalog first and unconditionally: a scope or API problem is
		// reported whether or not this particular glob found labelled files.
		LabelCatalog catalog;
		if (with_names) {
			catalog = FetchCatalog(*client);
		}
		// Rich rows: their capabilities.canReadLabels is what tells "no
		// labels" from "not allowed to see them" (Drive answers both with an
		// empty list).
		auto items = fs.ResolveItems(path, opener, /*rich=*/true, /*folder_as_item=*/false);
		auto data = make_uniq<RowsBindData>();
		for (auto &item : items) {
			if (item.meta.IsFolder()) {
				continue;
			}
			if (item.has_row && item.row.has_can_read_labels && !item.row.can_read_labels) {
				throw PermissionException(
				    "gdrive: cannot read the labels of '%s': Drive says this identity may not "
				    "(capabilities.canReadLabels is false), and would answer every such file with an empty list. "
				    "Labels need a Google Workspace edition that has them, enabled by the Workspace admin "
				    "(Admin console: Apps > Google Workspace > Drive and Docs > Labels) for this identity; a "
				    "service account outside the domain, or a file owned elsewhere, may not see them at all.",
				    item.path);
			}
			const std::string what = "the labels of '" + item.path + "'";
			std::string token;
			for (int pages = 0;; pages++) {
				if (pages >= kMaxPages) {
					throw IOException("gdrive: reading %s exceeded %d pages", what, kMaxPages);
				}
				std::string request = "/drive/v3/files/" + QueryEncode(item.meta.id) + "/listLabels?maxResults=100";
				if (!token.empty()) {
					request += "&pageToken=" + QueryEncode(token);
				}
				auto resp = client->ApiGet(request, &DriveCallStats::labels_list);
				if (!resp.ok) {
					ThrowGDriveError(resp.error, item.path);
				}
				std::vector<AppliedLabel> labels;
				std::string error;
				if (!ParseAppliedLabels(resp.body, labels, token, error)) {
					throw IOException("gdrive: unexpected response reading %s: %s", what, error);
				}
				for (auto &r : LabelRows(labels, with_names ? &catalog : nullptr)) {
					data->rows.push_back({Value(item.path), Value(item.meta.id), Value(r.label_id),
					                      TextOrNull(r.label), r.has_field ? Value(r.field_id) : Value(LogicalType::VARCHAR),
					                      TextOrNull(r.field), TextOrNull(r.value_type), TextOrNull(r.value)});
				}
				if (token.empty()) {
					break;
				}
			}
		}
		names = {"path", "id", "label_id", "label", "field_id", "field", "value_type", "value"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
		result = std::move(data);
	});
	return result;
}

} // namespace

void RegisterLabelFunctions(ExtensionLoader &loader) {
	TableFunction fn("gdrive_labels", {LogicalType::VARCHAR}, RowsScan, LabelsBind, RowsCursorInit);
	duckdb_compat::NameParameters(fn, {"path"});
	duckdb_compat::AddNamedParameter(fn, "names", LogicalType::BOOLEAN);
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "The Drive labels applied to a file, or to the files a glob or folder names: one row per (file, label, "
	    "field, value) -- path, id, label_id, label, field_id, field, value_type (text, integer, dateString, "
	    "selection, user), value (a user as their address). A label with no field set is one row with field NULL. "
	    "One files.listLabels call per file. names := true fills in label, field and selection choice names from "
	    "the Drive Labels API (one call per query), which needs the drive.labels.readonly scope and the API "
	    "enabled in the credential's Cloud project; without it label and field are NULL.";
	desc.examples = {"SELECT path, label_id, field_id, value FROM gdrive_labels('gdrive://Finance/**')",
	                 "SELECT path, label, field, value FROM gdrive_labels('gdrive://Finance/**', names := true)"};
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace gdrive
} // namespace duckdb
