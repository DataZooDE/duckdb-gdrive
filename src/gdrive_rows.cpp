// Drive API responses -> rows for the table functions. PURE: see gdrive_rows.hpp.
#include "gdrive_rows.hpp"

// jwt-cpp's picojson traits header, not picojson directly: it defines
// PICOJSON_USE_INT64 first, and mixing the two in one binary is the ODR trap
// described in CMakeLists.txt.
#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>

namespace duckdb {
namespace gdrive {

namespace {

const picojson::object *Obj(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<picojson::object>()) ? &it->second.get<picojson::object>() : nullptr;
}

const picojson::array *Arr(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<picojson::array>()) ? &it->second.get<picojson::array>() : nullptr;
}

std::string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<std::string>()) ? it->second.get<std::string>() : std::string();
}

bool Bool(const picojson::object &o, const char *key, bool &out) {
	auto it = o.find(key);
	if (it == o.end() || !it->second.is<bool>()) {
		return false;
	}
	out = it->second.get<bool>();
	return true;
}

//! Drive sends int64 fields (size) as decimal STRINGS, Google's convention for
//! values beyond a JavaScript safe integer.
int64_t SizeOf(const picojson::object &o, const char *key) {
	const auto s = Str(o, key);
	if (s.empty()) {
		return -1;
	}
	errno = 0;
	char *end = nullptr;
	const long long v = std::strtoll(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0' || errno == ERANGE || v < 0) {
		return -1;
	}
	return static_cast<int64_t>(v);
}

DrivePerson PersonOf(const picojson::object *o) {
	DrivePerson p;
	if (o) {
		p.name = Str(*o, "displayName");
		p.email = Str(*o, "emailAddress");
	}
	return p;
}

//! Parse a JSON document whose top level must be an object.
bool ParseTop(const std::string &json, picojson::value &holder, std::string &error) {
	const auto err = picojson::parse(holder, json);
	if (!err.empty()) {
		error = "malformed JSON: " + err;
		return false;
	}
	if (!holder.is<picojson::object>()) {
		error = "expected a JSON object";
		return false;
	}
	return true;
}

bool FileRowOf(const picojson::object &o, FileRow &out, std::string &error) {
	out.id = Str(o, "id");
	if (out.id.empty()) {
		error = "a file without an id";
		return false;
	}
	out.name = Str(o, "name");
	out.mime_type = Str(o, "mimeType");
	out.size = SizeOf(o, "size");
	out.created = Str(o, "createdTime");
	out.modified = Str(o, "modifiedTime");
	out.modified_by = PersonOf(Obj(o, "lastModifyingUser"));
	out.owners.clear();
	if (auto owners = Arr(o, "owners")) {
		for (auto &v : *owners) {
			if (v.is<picojson::object>()) {
				out.owners.push_back(PersonOf(&v.get<picojson::object>()));
			}
		}
	}
	out.web_url = Str(o, "webViewLink");
	out.md5 = Str(o, "md5Checksum");
	out.version = Str(o, "headRevisionId");
	out.description = Str(o, "description");
	out.parents.clear();
	if (auto parents = Arr(o, "parents")) {
		for (auto &v : *parents) {
			if (v.is<std::string>()) {
				out.parents.push_back(v.get<std::string>());
			}
		}
	}
	out.properties.clear();
	if (auto props = Obj(o, "properties")) {
		for (auto &kv : *props) {
			if (kv.second.is<std::string>()) {
				out.properties.emplace_back(kv.first, kv.second.get<std::string>());
			}
		}
		std::sort(out.properties.begin(), out.properties.end());
	}
	out.starred = false;
	out.shared = false;
	out.trashed = false;
	Bool(o, "starred", out.starred);
	Bool(o, "shared", out.shared);
	Bool(o, "trashed", out.trashed);
	out.locked = false;
	out.locked_by = DrivePerson();
	out.locked_at.clear();
	out.lock_reason.clear();
	if (auto restrictions = Arr(o, "contentRestrictions")) {
		// The first read-only entry is the lock. Drive keeps at most one
		// today; taking the first rather than the last keeps the answer
		// stable if it ever reports more.
		for (auto &v : *restrictions) {
			if (!v.is<picojson::object>()) {
				continue;
			}
			const auto &r = v.get<picojson::object>();
			bool read_only = false;
			if (Bool(r, "readOnly", read_only) && read_only) {
				out.locked = true;
				out.locked_by = PersonOf(Obj(r, "restrictingUser"));
				out.locked_at = Str(r, "restrictionTime");
				out.lock_reason = Str(r, "reason");
				break;
			}
		}
	}
	out.copy_requires_writer_permission = false;
	out.has_copy_requires_writer_permission =
	    Bool(o, "copyRequiresWriterPermission", out.copy_requires_writer_permission);
	out.can_read_labels = false;
	auto caps = Obj(o, "capabilities");
	out.has_can_read_labels = caps && Bool(*caps, "canReadLabels", out.can_read_labels);
	return true;
}

//! Walk the array under `key` of a list page, calling `each` per object.
template <class F>
bool ForEachItem(const picojson::object &top, const char *key, std::string &error, F each) {
	auto items = Arr(top, key);
	if (!items) {
		// An empty page carries the (empty) array; its absence means this is
		// not the response we asked for -- e.g. an error body.
		error = std::string("the response has no '") + key + "' array";
		return false;
	}
	for (auto &v : *items) {
		if (!v.is<picojson::object>()) {
			continue;
		}
		if (!each(v.get<picojson::object>())) {
			return false;
		}
	}
	return true;
}

int DaysFromCivil(int64_t y, unsigned m, unsigned d) {
	// Howard Hinnant's days_from_civil: proleptic Gregorian, valid for every
	// year Drive can produce.
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return static_cast<int>(era * 146097 + static_cast<int64_t>(doe) - 719468);
}

bool Digits(const std::string &s, size_t pos, size_t n, int &out) {
	if (pos + n > s.size()) {
		return false;
	}
	out = 0;
	for (size_t i = pos; i < pos + n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
		out = out * 10 + (s[i] - '0');
	}
	return true;
}

} // namespace

bool ParseRfc3339Micros(const std::string &s, int64_t &micros) {
	// YYYY-MM-DDTHH:MM:SS[.f+](Z|+HH:MM|-HH:MM)
	int year, month, day, hour, minute, second;
	if (!Digits(s, 0, 4, year) || s.size() < 19 || s[4] != '-' || !Digits(s, 5, 2, month) || s[7] != '-' ||
	    !Digits(s, 8, 2, day) || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') || !Digits(s, 11, 2, hour) ||
	    s[13] != ':' || !Digits(s, 14, 2, minute) || s[16] != ':' || !Digits(s, 17, 2, second)) {
		return false;
	}
	if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
		return false;
	}
	size_t pos = 19;
	int64_t fraction = 0;
	if (pos < s.size() && s[pos] == '.') {
		pos++;
		int digits = 0;
		while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
			if (digits < 6) {
				fraction = fraction * 10 + (s[pos] - '0');
			}
			digits++;
			pos++;
		}
		if (digits == 0) {
			return false;
		}
		for (int i = digits; i < 6; i++) {
			fraction *= 10;
		}
	}
	int64_t offset_seconds = 0;
	if (pos < s.size() && (s[pos] == 'Z' || s[pos] == 'z')) {
		pos++;
	} else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
		int oh, om;
		if (!Digits(s, pos + 1, 2, oh) || pos + 3 >= s.size() || s[pos + 3] != ':' || !Digits(s, pos + 4, 2, om)) {
			return false;
		}
		offset_seconds = (oh * 3600 + om * 60) * (s[pos] == '+' ? 1 : -1);
		pos += 6;
	} else {
		return false;
	}
	if (pos != s.size()) {
		return false;
	}
	const int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
	const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
	micros = seconds * 1000000 + fraction;
	return true;
}

bool FileRow::IsFolder() const {
	return mime_type == "application/vnd.google-apps.folder";
}

std::string FileRowFields() {
	return "id,name,mimeType,size,createdTime,modifiedTime,lastModifyingUser(displayName,emailAddress),"
	       "owners(displayName,emailAddress),webViewLink,md5Checksum,headRevisionId,description,parents,"
	       "properties,starred,shared,trashed,copyRequiresWriterPermission,"
	       "contentRestrictions(readOnly,reason,restrictionTime,restrictingUser(displayName,emailAddress)),"
	       "capabilities(canReadLabels)";
}

bool ParseFileRow(const std::string &json_object, FileRow &out, std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_object, holder, error)) {
		return false;
	}
	return FileRowOf(holder.get<picojson::object>(), out, error);
}

bool ParseFileRowList(const std::string &json_body, std::vector<FileRow> &out, std::string &next_page_token,
                      std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	const auto &top = holder.get<picojson::object>();
	next_page_token = Str(top, "nextPageToken");
	return ForEachItem(top, "files", error, [&](const picojson::object &o) {
		FileRow row;
		if (!FileRowOf(o, row, error)) {
			return false;
		}
		out.push_back(std::move(row));
		return true;
	});
}

std::string PermissionFields() {
	return "id,type,role,emailAddress,domain,displayName,expirationTime,allowFileDiscovery,deleted,"
	       "permissionDetails(inherited,inheritedFrom)";
}

bool ParsePermissions(const std::string &json_body, std::vector<PermissionRow> &out, std::string &next_page_token,
                      std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	const auto &top = holder.get<picojson::object>();
	next_page_token = Str(top, "nextPageToken");
	return ForEachItem(top, "permissions", error, [&](const picojson::object &o) {
		PermissionRow row;
		row.permission_id = Str(o, "id");
		row.role = Str(o, "role");
		row.grantee_type = Str(o, "type");
		row.grantee_email = Str(o, "emailAddress");
		row.grantee = row.grantee_type == "domain" ? Str(o, "domain") : Str(o, "displayName");
		row.expires = Str(o, "expirationTime");
		row.has_discoverable = Bool(o, "allowFileDiscovery", row.discoverable);
		if (row.grantee_type == "anyone" || row.grantee_type == "domain") {
			row.granted_by = "link";
			row.link_scope = row.grantee_type == "anyone" ? "anonymous" : "organization";
		} else {
			// Inherited only when NOTHING is granted on the item itself: an
			// owner who also inherits writer from the folder has a direct grant.
			bool any_detail = false;
			bool all_inherited = true;
			if (auto details = Arr(o, "permissionDetails")) {
				for (auto &v : *details) {
					if (!v.is<picojson::object>()) {
						continue;
					}
					const auto &d = v.get<picojson::object>();
					any_detail = true;
					bool inherited = false;
					Bool(d, "inherited", inherited);
					all_inherited = all_inherited && inherited;
					if (inherited && row.inherited_from.empty()) {
						row.inherited_from = Str(d, "inheritedFrom");
					}
				}
			}
			row.granted_by = any_detail && all_inherited ? "inherited" : "direct";
			if (row.granted_by == "direct") {
				row.inherited_from.clear();
			}
		}
		out.push_back(std::move(row));
		return true;
	});
}

std::string RevisionFields() {
	return "id,modifiedTime,size,md5Checksum,mimeType,keepForever,lastModifyingUser(displayName,emailAddress)";
}

namespace {
bool RevisionOf(const picojson::object &o, RevisionRow &row, std::string &error) {
	row.id = Str(o, "id");
	if (row.id.empty()) {
		error = "a revision without an id";
		return false;
	}
	row.modified = Str(o, "modifiedTime");
	row.modified_by = PersonOf(Obj(o, "lastModifyingUser"));
	row.size = SizeOf(o, "size");
	row.md5 = Str(o, "md5Checksum");
	row.mime_type = Str(o, "mimeType");
	row.keep_forever = false;
	Bool(o, "keepForever", row.keep_forever);
	return true;
}
} // namespace

bool ParseRevision(const std::string &json_object, RevisionRow &out, std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_object, holder, error)) {
		return false;
	}
	return RevisionOf(holder.get<picojson::object>(), out, error);
}

bool ParseRevisions(const std::string &json_body, std::vector<RevisionRow> &out, std::string &next_page_token,
                    std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	const auto &top = holder.get<picojson::object>();
	next_page_token = Str(top, "nextPageToken");
	return ForEachItem(top, "revisions", error, [&](const picojson::object &o) {
		RevisionRow row;
		if (!RevisionOf(o, row, error)) {
			return false;
		}
		out.push_back(std::move(row));
		return true;
	});
}

std::string SharedDriveFields() {
	return "id,name,createdTime,hidden";
}

bool ParseSharedDrives(const std::string &json_body, std::vector<SharedDriveRow> &out, std::string &next_page_token,
                       std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	const auto &top = holder.get<picojson::object>();
	next_page_token = Str(top, "nextPageToken");
	return ForEachItem(top, "drives", error, [&](const picojson::object &o) {
		SharedDriveRow row;
		row.id = Str(o, "id");
		row.name = Str(o, "name");
		row.created = Str(o, "createdTime");
		Bool(o, "hidden", row.hidden);
		out.push_back(std::move(row));
		return true;
	});
}

std::string ChangeFields() {
	return "changeType,fileId,removed,time,file(" + FileRowFields() + ")";
}

bool ParseChanges(const std::string &json_body, std::vector<ChangeRow> &out, std::string &next_page_token,
                  std::string &new_start_page_token, std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	const auto &top = holder.get<picojson::object>();
	next_page_token = Str(top, "nextPageToken");
	new_start_page_token = Str(top, "newStartPageToken");
	if (next_page_token.empty() && new_start_page_token.empty()) {
		// Every page carries one or the other; a page with neither would leave
		// the caller with no way to continue and no token to keep.
		error = "the response carries neither nextPageToken nor newStartPageToken";
		return false;
	}
	return ForEachItem(top, "changes", error, [&](const picojson::object &o) {
		const auto type = Str(o, "changeType");
		if (!type.empty() && type != "file") {
			return true;
		}
		ChangeRow row;
		row.file_id = Str(o, "fileId");
		if (row.file_id.empty()) {
			error = "a change without a fileId";
			return false;
		}
		row.time = Str(o, "time");
		Bool(o, "removed", row.removed);
		if (auto file = Obj(o, "file")) {
			if (!FileRowOf(*file, row.file, error)) {
				return false;
			}
			row.has_file = true;
		}
		out.push_back(std::move(row));
		return true;
	});
}

bool ParseStartPageToken(const std::string &json_body, std::string &token, std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	token = Str(holder.get<picojson::object>(), "startPageToken");
	if (token.empty()) {
		error = "the response has no startPageToken";
		return false;
	}
	return true;
}

} // namespace gdrive
} // namespace duckdb
