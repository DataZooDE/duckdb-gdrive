#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Drive API responses -> rows, for the table functions (read_gdrive_files,
// search_gdrive, gdrive_drives, gdrive_permissions, gdrive_versions,
// gdrive_changes). PURE: no duckdb.hpp, no I/O, never throws -- each parser
// returns false with `error` set, and the DuckDB-side caller turns that into
// an exception naming what was being read.
//
// The structs keep Drive's own text (RFC 3339 timestamps, decimal size
// strings already converted) so the DuckDB side does the typing in one place.
// Field masks live here too, next to the parser that consumes them: a field
// the mask does not ask for is a column that is silently always NULL.
// ---------------------------------------------------------------------------

//! Microseconds since the Unix epoch for an RFC 3339 timestamp as Drive writes
//! it ("2026-07-26T14:31:31.355Z", or with a "+02:00" offset). False when
//! malformed. The result is UTC, which is what the TIMESTAMP columns carry.
bool ParseRfc3339Micros(const std::string &text, int64_t &micros);

//! A user as Drive reports one (lastModifyingUser, owners[], revisions).
//! Either field may be empty: Drive omits the address of a user who hides it.
struct DrivePerson {
	std::string name;
	std::string email;
};

//! One file or folder, with everything read_gdrive_files and search_gdrive
//! return. `size` is -1 when Drive reported none (folders, some native files).
struct FileRow {
	std::string id;
	std::string name;
	std::string mime_type;
	int64_t size = -1;
	std::string created;
	std::string modified;
	DrivePerson modified_by;
	std::vector<DrivePerson> owners;
	std::string web_url;
	std::string md5;
	//! headRevisionId; empty for native Google formats, which have none.
	std::string version;
	std::string description;
	std::vector<std::string> parents;
	//! Drive's custom `properties` (public key/value metadata), sorted by key.
	std::vector<std::pair<std::string, std::string>> properties;
	bool starred = false;
	bool shared = false;
	bool trashed = false;

	bool IsFolder() const;
};

//! The `files(...)` fields read_gdrive_files and search_gdrive need, without
//! the `files(` wrapper -- usable as files.get's `fields` directly.
std::string FileRowFields();

bool ParseFileRow(const std::string &json_object, FileRow &out, std::string &error);
//! A files.list page. `next_page_token` is empty on the last page.
bool ParseFileRowList(const std::string &json_body, std::vector<FileRow> &out, std::string &next_page_token,
                      std::string &error);

//! One permission on a file or folder.
//!
//! Drive has four permission types and one role per permission. Mapped onto
//! the sibling duckdb-sharepoint's vocabulary so an audit reads the same:
//!  - `granted_by`: "link" for the `anyone` / `domain` types (general access:
//!    whoever has the address, or everyone in a domain), else "inherited"
//!    when every permissionDetails entry says inherited, else "direct".
//!  - `link_scope`: "anonymous" for `anyone`, "organization" for `domain`,
//!    empty otherwise.
struct PermissionRow {
	std::string permission_id;
	//! owner, organizer, fileOrganizer, writer, commenter, reader.
	std::string role;
	std::string granted_by;
	//! user, group, domain, anyone.
	std::string grantee_type;
	//! displayName for a user or group, the domain for `domain`.
	std::string grantee;
	std::string grantee_email;
	std::string link_scope;
	//! allowFileDiscovery: for `anyone` / `domain`, whether the file turns up
	//! in search, rather than only for people who have its address.
	bool discoverable = false;
	bool has_discoverable = false;
	//! Shared Drives only: the folder id the permission comes from.
	std::string inherited_from;
	std::string expires;
};

std::string PermissionFields();
bool ParsePermissions(const std::string &json_body, std::vector<PermissionRow> &out, std::string &next_page_token,
                      std::string &error);

//! One stored revision of a file (revisions.list). Drive lists oldest first.
struct RevisionRow {
	std::string id;
	std::string modified;
	DrivePerson modified_by;
	int64_t size = -1;
	std::string md5;
	std::string mime_type;
	bool keep_forever = false;
};

std::string RevisionFields();
bool ParseRevisions(const std::string &json_body, std::vector<RevisionRow> &out, std::string &next_page_token,
                    std::string &error);

//! One Shared Drive (drives.list).
struct SharedDriveRow {
	std::string id;
	std::string name;
	std::string created;
	bool hidden = false;
};

std::string SharedDriveFields();
bool ParseSharedDrives(const std::string &json_body, std::vector<SharedDriveRow> &out, std::string &next_page_token,
                       std::string &error);

//! One entry of the changes feed (changes.list). `removed` entries carry no
//! file: Drive says only that the id is gone (deleted, or no longer visible
//! to this identity). A trashed file is NOT removed -- it still has a file
//! with trashed=true, and the caller treats it as a delete.
struct ChangeRow {
	std::string file_id;
	std::string time;
	bool removed = false;
	bool has_file = false;
	FileRow file;
	//! Drive also reports Shared Drive changes (changeType "drive"); those
	//! are not about a file and are skipped by the parser.
};

std::string ChangeFields();
//! A changes.list page: `next_page_token` while more pages follow, then
//! `new_start_page_token` on the last one -- the token for next time.
bool ParseChanges(const std::string &json_body, std::vector<ChangeRow> &out, std::string &next_page_token,
                  std::string &new_start_page_token, std::string &error);

//! changes.getStartPageToken's answer.
bool ParseStartPageToken(const std::string &json_body, std::string &token, std::string &error);

} // namespace gdrive
} // namespace duckdb
