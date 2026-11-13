// Drive API responses -> table-function rows. Pure logic, no DuckDB linkage.
//
// The bodies below are CAPTURED from the live Drive API (2026-11-07, as the CI
// service account against the fixture folder), not written by hand. Only the
// e-mail addresses and display names are replaced. Data, not mocks: there is
// nothing here pretending to be Drive, only what Drive said.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "gdrive_rows.hpp"

using namespace duckdb::gdrive;

namespace {

const char *kFilesGet = R"({
 "id": "18Yue0IodacKje3gEl632Pkaw7nc_ZVSX",
 "name": "small.csv",
 "mimeType": "text/csv",
 "starred": false,
 "trashed": false,
 "parents": ["1faIU3KNnDKJKsTf_81GxNBLhYV7kpDX-"],
 "webViewLink": "https://drive.google.com/file/d/18Yue0IodacKje3gEl632Pkaw7nc_ZVSX/view?usp=drivesdk",
 "createdTime": "2026-07-26T14:31:31.355Z",
 "modifiedTime": "2026-07-26T14:31:31.355Z",
 "owners": [{"displayName": "owner", "emailAddress": "owner@example.com"}],
 "lastModifyingUser": {"displayName": "owner", "emailAddress": "owner@example.com"},
 "shared": true,
 "capabilities": {"canEdit": true},
 "md5Checksum": "b420c535a6c6c3574d1599f8df6df115",
 "size": "75",
 "headRevisionId": "0B9fS7-FFsZTlL285U1lpZ0dGWUtYd3NnM0lST1ZFMlVUaEFNPQ"
})";

const char *kFilesList = R"({
 "files": [
  {"mimeType": "application/vnd.google-apps.folder", "parents": ["1faIU3KNnDKJKsTf_81GxNBLhYV7kpDX-"],
   "shared": true, "lastModifyingUser": {"displayName": "owner", "emailAddress": "owner@example.com"},
   "owners": [{"displayName": "owner", "emailAddress": "owner@example.com"}],
   "webViewLink": "https://drive.google.com/drive/folders/1XG82dbbIb9KBOda8_iQrcfouOpTCmhjs",
   "id": "1XG82dbbIb9KBOda8_iQrcfouOpTCmhjs", "name": "hive", "starred": false, "trashed": false,
   "createdTime": "2026-07-26T18:00:18.142Z", "modifiedTime": "2026-07-26T18:00:18.142Z"},
  {"mimeType": "application/vnd.google-apps.document", "parents": ["1faIU3KNnDKJKsTf_81GxNBLhYV7kpDX-"],
   "shared": true, "webViewLink": "https://docs.google.com/document/d/1pNWvcXErPx4e6WjDLZi6M3Me4emePxKoq4K2huKPfqM/edit?usp=drivesdk",
   "size": "1024", "id": "1pNWvcXErPx4e6WjDLZi6M3Me4emePxKoq4K2huKPfqM", "name": "Notes", "starred": false,
   "trashed": false, "createdTime": "2026-07-26T17:24:41.588Z", "modifiedTime": "2026-07-26T17:24:43.325Z",
   "properties": {"stage": "draft", "owner_team": "finance"}}
 ],
 "nextPageToken": "~!!~AI9FV7T"
})";

const char *kPermissions = R"({
 "permissions": [
  {"id": "03795213208351066783", "displayName": "owner", "type": "user",
   "permissionDetails": [{"permissionType": "file", "role": "writer", "inherited": true},
                         {"permissionType": "file", "role": "owner", "inherited": false}],
   "emailAddress": "owner@example.com", "role": "owner", "deleted": false},
  {"id": "15323764974608315463", "displayName": "reader@example-project.iam.gserviceaccount.com", "type": "user",
   "permissionDetails": [{"permissionType": "file", "role": "writer", "inherited": true}],
   "emailAddress": "reader@example-project.iam.gserviceaccount.com", "role": "writer", "deleted": false}
 ]
})";

const char *kRevisions = R"({
 "revisions": [
  {"lastModifyingUser": {"displayName": "owner", "emailAddress": "owner@example.com"},
   "id": "0B9fS7-FFsZTlL285U1lpZ0dGWUtYd3NnM0lST1ZFMlVUaEFNPQ", "mimeType": "text/csv",
   "modifiedTime": "2026-07-26T14:31:31.355Z", "keepForever": false,
   "md5Checksum": "b420c535a6c6c3574d1599f8df6df115", "size": "75"}
 ]
})";

const char *kChanges = R"({
 "changes": [
  {"changeType": "file", "time": "2026-07-26T06:06:20.729Z", "removed": true,
   "fileId": "1Z4h0u2QxYHKug3bJ_ff3NdjqOLyTEMxl"},
  {"changeType": "file", "time": "2026-07-26T14:31:31.355Z", "removed": false,
   "fileId": "18Yue0IodacKje3gEl632Pkaw7nc_ZVSX",
   "file": {"id": "18Yue0IodacKje3gEl632Pkaw7nc_ZVSX", "name": "small.csv", "mimeType": "text/csv",
            "trashed": false, "parents": ["1faIU3KNnDKJKsTf_81GxNBLhYV7kpDX-"], "size": "75"}},
  {"changeType": "drive", "time": "2026-07-26T14:31:31.355Z", "driveId": "0AGdCBrEPvfZpUk9PVA"}
 ],
 "newStartPageToken": "86974"
})";

// A locked file (contentRestrictions readOnly), captured 2026-11-13 in a
// scratch folder: files.get as the delegated user who locked it, and the same
// file listed by the service account -- which Drive shows only a display name
// and no address for the restricting user.
const char *kLockedGet = R"({
  "id": "1etXYhzfAk2Azh0neD45f82U6oyeHTi-7",
  "name": "locked.csv",
  "capabilities": {
    "canModifyContentRestriction": true,
    "canModifyEditorContentRestriction": true,
    "canModifyOwnerContentRestriction": true,
    "canModifyLabels": false,
    "canReadLabels": false,
    "canRemoveContentRestriction": true
  },
  "copyRequiresWriterPermission": true,
  "contentRestrictions": [
    {
      "readOnly": true,
      "reason": "Quarter closed",
      "restrictingUser": {
        "displayName": "owner",
        "emailAddress": "owner@example.com"
      },
      "restrictionTime": "2026-11-13T03:38:35.503Z",
      "type": "globalContentRestriction"
    }
  ]
})";

const char *kLockedListedBySa = R"({
  "files": [
    {
      "copyRequiresWriterPermission": true,
      "id": "1etXYhzfAk2Azh0neD45f82U6oyeHTi-7",
      "name": "locked.csv",
      "contentRestrictions": [
        {
          "readOnly": true,
          "reason": "Quarter closed",
          "type": "globalContentRestriction",
          "restrictingUser": {
            "displayName": "owner"
          },
          "restrictionTime": "2026-11-13T03:38:35.503Z"
        }
      ]
    }
  ]
})";

// The same file after files.update {contentRestrictions: [{readOnly: false}]}:
// Drive drops the array altogether rather than reporting readOnly=false.
const char *kUnlockedGet = R"({
  "id": "1etXYhzfAk2Azh0neD45f82U6oyeHTi-7",
  "name": "locked.csv",
  "capabilities": {
    "canModifyContentRestriction": true,
    "canModifyEditorContentRestriction": true,
    "canModifyOwnerContentRestriction": true,
    "canModifyLabels": false,
    "canReadLabels": false,
    "canRemoveContentRestriction": false
  },
  "copyRequiresWriterPermission": false
})";

} // namespace

TEST_CASE("ParseRfc3339Micros: Drive's UTC form, fractions and offsets", "[rows][time]") {
	int64_t us = 0;
	REQUIRE(ParseRfc3339Micros("1970-01-01T00:00:00Z", us));
	CHECK(us == 0);
	REQUIRE(ParseRfc3339Micros("2026-07-26T14:31:31.355Z", us));
	CHECK(us == 1785076291355000LL);
	// An offset is converted to UTC, not dropped.
	REQUIRE(ParseRfc3339Micros("2026-07-26T16:31:31.355+02:00", us));
	CHECK(us == 1785076291355000LL);
	// More than microsecond precision is truncated, fewer digits are scaled.
	REQUIRE(ParseRfc3339Micros("2026-07-26T14:31:31.3550009Z", us));
	CHECK(us == 1785076291355000LL);
	REQUIRE(ParseRfc3339Micros("2026-07-26T14:31:31.3Z", us));
	CHECK(us == 1785076291300000LL);
	// Before the epoch.
	REQUIRE(ParseRfc3339Micros("1969-12-31T23:59:59Z", us));
	CHECK(us == -1000000LL);
}

TEST_CASE("ParseRfc3339Micros: rejects what is not a timestamp", "[rows][time]") {
	int64_t us = 0;
	CHECK_FALSE(ParseRfc3339Micros("", us));
	CHECK_FALSE(ParseRfc3339Micros("2026-07-26", us));
	CHECK_FALSE(ParseRfc3339Micros("2026-07-26T14:31:31", us)); // no zone: ambiguous
	CHECK_FALSE(ParseRfc3339Micros("2026-13-26T14:31:31Z", us));
	CHECK_FALSE(ParseRfc3339Micros("2026-07-26T14:31:31.Z", us));
	CHECK_FALSE(ParseRfc3339Micros("2026-07-26T14:31:31Zjunk", us));
	CHECK_FALSE(ParseRfc3339Micros("2026-07-26T14:31:31+0200", us));
}

TEST_CASE("ParseFileRow: every column read_gdrive_files returns", "[rows][files]") {
	FileRow row;
	std::string error;
	REQUIRE(ParseFileRow(kFilesGet, row, error));
	CHECK(row.id == "18Yue0IodacKje3gEl632Pkaw7nc_ZVSX");
	CHECK(row.name == "small.csv");
	CHECK(row.mime_type == "text/csv");
	CHECK(row.size == 75); // a STRING on the wire
	CHECK(row.created == "2026-07-26T14:31:31.355Z");
	CHECK(row.modified_by.name == "owner");
	CHECK(row.modified_by.email == "owner@example.com");
	REQUIRE(row.owners.size() == 1);
	CHECK(row.owners[0].email == "owner@example.com");
	CHECK(row.web_url == "https://drive.google.com/file/d/18Yue0IodacKje3gEl632Pkaw7nc_ZVSX/view?usp=drivesdk");
	CHECK(row.md5 == "b420c535a6c6c3574d1599f8df6df115");
	CHECK(row.version == "0B9fS7-FFsZTlL285U1lpZ0dGWUtYd3NnM0lST1ZFMlVUaEFNPQ");
	CHECK(row.parents == std::vector<std::string> {"1faIU3KNnDKJKsTf_81GxNBLhYV7kpDX-"});
	CHECK(row.shared);
	CHECK_FALSE(row.starred);
	CHECK_FALSE(row.trashed);
	CHECK_FALSE(row.IsFolder());
}

TEST_CASE("ParseFileRowList: folders, native files, properties and the page token", "[rows][files]") {
	std::vector<FileRow> rows;
	std::string next, error;
	REQUIRE(ParseFileRowList(kFilesList, rows, next, error));
	CHECK(next == "~!!~AI9FV7T");
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].IsFolder());
	CHECK(rows[0].size == -1); // folders have no size
	CHECK(rows[0].version.empty());
	// A native Doc reports a size (here 1024) but no md5 and no revision.
	CHECK(rows[1].size == 1024);
	CHECK(rows[1].md5.empty());
	CHECK(rows[1].version.empty());
	CHECK(rows[1].modified_by.email.empty()); // absent, not an error
	REQUIRE(rows[1].properties.size() == 2);
	CHECK(rows[1].properties[0] == std::make_pair(std::string("owner_team"), std::string("finance"))); // sorted
	CHECK(rows[1].properties[1] == std::make_pair(std::string("stage"), std::string("draft")));
}

TEST_CASE("ParseFileRowList: an error body is not an empty listing", "[rows][files]") {
	std::vector<FileRow> rows;
	std::string next, error;
	// An empty page is a valid answer...
	REQUIRE(ParseFileRowList(R"({"files": []})", rows, next, error));
	CHECK(rows.empty());
	// ...a body without the array is not: reporting it as "no files" would
	// turn a failure into a silently empty result.
	CHECK_FALSE(ParseFileRowList(R"({"error": {"code": 403}})", rows, next, error));
	CHECK(error.find("'files'") != std::string::npos);
	CHECK_FALSE(ParseFileRowList("not json", rows, next, error));
	CHECK_FALSE(ParseFileRowList(R"([1,2])", rows, next, error));
}

TEST_CASE("ParsePermissions: direct beats inherited; links get a scope", "[rows][permissions]") {
	std::vector<PermissionRow> rows;
	std::string next, error;
	REQUIRE(ParsePermissions(kPermissions, rows, next, error));
	CHECK(next.empty());
	REQUIRE(rows.size() == 2);
	// The owner also inherits writer from the folder, but owns the file
	// itself: that is a direct grant.
	CHECK(rows[0].role == "owner");
	CHECK(rows[0].granted_by == "direct");
	CHECK(rows[0].grantee_type == "user");
	CHECK(rows[0].grantee_email == "owner@example.com");
	CHECK(rows[0].link_scope.empty());
	CHECK(rows[1].role == "writer");
	CHECK(rows[1].granted_by == "inherited");

	rows.clear();
	REQUIRE(ParsePermissions(R"({"permissions": [
	  {"id": "anyoneWithLink", "type": "anyone", "role": "reader", "allowFileDiscovery": false},
	  {"id": "0912", "type": "domain", "role": "commenter", "domain": "example.com", "allowFileDiscovery": true,
	   "expirationTime": "2026-12-31T00:00:00.000Z"},
	  {"id": "1234", "type": "group", "role": "reader", "displayName": "Finance", "emailAddress": "fin@example.com",
	   "permissionDetails": [{"inherited": true, "inheritedFrom": "0AGdCBrEPvfZpUk9PVA"}]}]})",
	                         rows, next, error));
	REQUIRE(rows.size() == 3);
	CHECK(rows[0].granted_by == "link");
	CHECK(rows[0].link_scope == "anonymous");
	CHECK(rows[0].has_discoverable);
	CHECK_FALSE(rows[0].discoverable);
	CHECK(rows[1].link_scope == "organization");
	CHECK(rows[1].grantee == "example.com");
	CHECK(rows[1].discoverable);
	CHECK(rows[1].expires == "2026-12-31T00:00:00.000Z");
	CHECK(rows[2].granted_by == "inherited");
	CHECK(rows[2].inherited_from == "0AGdCBrEPvfZpUk9PVA");
	CHECK_FALSE(rows[2].has_discoverable);
}

TEST_CASE("ParseRevisions: id, size and who", "[rows][versions]") {
	std::vector<RevisionRow> rows;
	std::string next, error;
	REQUIRE(ParseRevisions(kRevisions, rows, next, error));
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].id == "0B9fS7-FFsZTlL285U1lpZ0dGWUtYd3NnM0lST1ZFMlVUaEFNPQ");
	CHECK(rows[0].size == 75);
	CHECK(rows[0].md5 == "b420c535a6c6c3574d1599f8df6df115");
	CHECK(rows[0].modified_by.email == "owner@example.com");
	CHECK_FALSE(rows[0].keep_forever);
}

TEST_CASE("ParseSharedDrives: an identity in no Shared Drive gets an empty array", "[rows][drives]") {
	std::vector<SharedDriveRow> rows;
	std::string next, error;
	REQUIRE(ParseSharedDrives(R"({"drives": []})", rows, next, error));
	CHECK(rows.empty());
	REQUIRE(ParseSharedDrives(R"({"drives": [{"id": "0AGdCBrEPvfZpUk9PVA", "name": "Customers and Sales",
	                              "createdTime": "2025-01-02T03:04:05.000Z", "hidden": true}]})",
	                          rows, next, error));
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].name == "Customers and Sales");
	CHECK(rows[0].hidden);
}

TEST_CASE("ParseChanges: removals, files, and the token for next time", "[rows][changes]") {
	std::vector<ChangeRow> rows;
	std::string next, start, error;
	REQUIRE(ParseChanges(kChanges, rows, next, start, error));
	CHECK(next.empty());
	CHECK(start == "86974");
	// The Shared Drive change is not about a file and is skipped.
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].removed);
	CHECK_FALSE(rows[0].has_file);
	CHECK(rows[0].file_id == "1Z4h0u2QxYHKug3bJ_ff3NdjqOLyTEMxl");
	CHECK_FALSE(rows[1].removed);
	REQUIRE(rows[1].has_file);
	CHECK(rows[1].file.name == "small.csv");
	CHECK(rows[1].file.parents.size() == 1);
}

TEST_CASE("ParseChanges: a page with no way forward is an error", "[rows][changes]") {
	std::vector<ChangeRow> rows;
	std::string next, start, error;
	CHECK_FALSE(ParseChanges(R"({"changes": []})", rows, next, start, error));
	REQUIRE(ParseChanges(R"({"changes": [], "nextPageToken": "5"})", rows, next, start, error));
	CHECK(next == "5");
}

TEST_CASE("ParseStartPageToken", "[rows][changes]") {
	std::string token, error;
	REQUIRE(ParseStartPageToken(R"({"kind": "drive#startPageToken", "startPageToken": "86974"})", token, error));
	CHECK(token == "86974");
	CHECK_FALSE(ParseStartPageToken(R"({"kind": "drive#startPageToken"})", token, error));
}

TEST_CASE("ParseFileRow: a content restriction is the lock", "[rows][files][locks]") {
	FileRow row;
	std::string error;
	REQUIRE(ParseFileRow(kLockedGet, row, error));
	CHECK(row.locked);
	CHECK(row.locked_by.name == "owner");
	CHECK(row.locked_by.email == "owner@example.com");
	CHECK(row.locked_at == "2026-11-13T03:38:35.503Z");
	CHECK(row.lock_reason == "Quarter closed");
	CHECK(row.has_copy_requires_writer_permission);
	CHECK(row.copy_requires_writer_permission);
	// Captured in a Workspace with no labels enabled for this identity.
	CHECK(row.has_can_read_labels);
	CHECK_FALSE(row.can_read_labels);
}

TEST_CASE("ParseFileRowList: a service account sees who locked a file, not their address", "[rows][files][locks]") {
	std::vector<FileRow> rows;
	std::string next, error;
	REQUIRE(ParseFileRowList(kLockedListedBySa, rows, next, error));
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].locked);
	CHECK(rows[0].locked_by.name == "owner");
	CHECK(rows[0].locked_by.email.empty());
	CHECK(rows[0].lock_reason == "Quarter closed");
}

TEST_CASE("ParseFileRow: unlocked, and never asked", "[rows][files][locks]") {
	FileRow row;
	std::string error;
	REQUIRE(ParseFileRow(kUnlockedGet, row, error));
	CHECK_FALSE(row.locked);
	CHECK(row.locked_by.name.empty());
	CHECK(row.locked_at.empty());
	CHECK(row.lock_reason.empty());
	CHECK(row.has_copy_requires_writer_permission);
	CHECK_FALSE(row.copy_requires_writer_permission);
	// A body from before the mask asked for either field: unlocked, and the
	// copy flag unknown (NULL), not false.
	REQUIRE(ParseFileRow(kFilesGet, row, error));
	CHECK_FALSE(row.locked);
	CHECK_FALSE(row.has_copy_requires_writer_permission);
}
