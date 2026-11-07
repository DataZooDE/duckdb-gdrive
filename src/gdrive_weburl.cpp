// Browser addresses as paths. PURE: see gdrive_weburl.hpp.
#include "gdrive_weburl.hpp"

#include <cctype>
#include <cstring>
#include <vector>

namespace duckdb {
namespace gdrive {

namespace {

const char *const kHosts[] = {"drive.google.com", "docs.google.com", "drive.usercontent.google.com"};

//! Length of "http://" or "https://" at the start, else 0.
size_t SchemeLength(const std::string &url) {
	if (url.compare(0, 8, "https://") == 0) {
		return 8;
	}
	if (url.compare(0, 7, "http://") == 0) {
		return 7;
	}
	return 0;
}

bool IsIdChar(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool IsId(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		if (!IsIdChar(c)) {
			return false;
		}
	}
	return true;
}

std::vector<std::string> Split(const std::string &s, char sep) {
	std::vector<std::string> out;
	size_t start = 0;
	for (;;) {
		auto at = s.find(sep, start);
		auto part = s.substr(start, at == std::string::npos ? std::string::npos : at - start);
		if (!part.empty()) {
			out.push_back(part);
		}
		if (at == std::string::npos) {
			return out;
		}
		start = at + 1;
	}
}

//! The value of `key` in a query string ("a=1&id=x"), else empty.
std::string QueryValue(const std::string &query, const std::string &key) {
	for (auto &pair : Split(query, '&')) {
		auto eq = pair.find('=');
		if (eq != std::string::npos && pair.compare(0, eq, key) == 0 && eq == key.size()) {
			return pair.substr(eq + 1);
		}
	}
	return std::string();
}

} // namespace

namespace {
//! Hosts are case-insensitive: DRIVE.GOOGLE.COM is drive.google.com.
bool HostAt(const std::string &url, size_t at, const char *host) {
	const size_t n = std::strlen(host);
	if (url.size() < at + n) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (std::tolower(static_cast<unsigned char>(url[at + i])) != host[i]) {
			return false;
		}
	}
	return url.size() == at + n || url[at + n] == '/' || url[at + n] == '?';
}
} // namespace

bool IsDriveWebUrl(const std::string &url) {
	const size_t scheme = SchemeLength(url);
	if (scheme == 0) {
		return false;
	}
	for (auto host : kHosts) {
		if (HostAt(url, scheme, host)) {
			return true;
		}
	}
	return false;
}

DriveWebUrl ParseDriveWebUrl(const std::string &url) {
	DriveWebUrl out;
	if (!IsDriveWebUrl(url)) {
		out.error = "not a Google Drive address: \"" + url + "\"";
		return out;
	}
	auto rest = url.substr(SchemeLength(url));
	auto slash = rest.find_first_of("/?");
	auto host = rest.substr(0, slash);
	for (auto &c : host) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	rest = slash == std::string::npos ? std::string() : rest.substr(slash);
	// Fragment (#gid=0) and query apart from the path.
	auto hash = rest.find('#');
	if (hash != std::string::npos) {
		rest = rest.substr(0, hash);
	}
	std::string query;
	auto q = rest.find('?');
	if (q != std::string::npos) {
		query = rest.substr(q + 1);
		rest = rest.substr(0, q);
	}
	// Path segments KEEP their empties: "/file/d//view" must not read "view"
	// as the id. Only the leading slash and a trailing one are dropped.
	std::vector<std::string> parts;
	{
		size_t start = rest.empty() || rest[0] != '/' ? 0 : 1;
		while (start <= rest.size()) {
			auto at = rest.find('/', start);
			parts.push_back(rest.substr(start, at == std::string::npos ? std::string::npos : at - start));
			if (at == std::string::npos) {
				break;
			}
			start = at + 1;
		}
		if (!parts.empty() && parts.back().empty()) {
			parts.pop_back();
		}
	}
	// Legacy Workspace prefix: /a/<domain>/...
	if (parts.size() >= 2 && parts[0] == "a") {
		parts.erase(parts.begin(), parts.begin() + 2);
	}
	// Account selector: /u/<n>/... anywhere before the id.
	for (size_t i = 0; i + 1 < parts.size(); i++) {
		if (parts[i] == "u" && !parts[i + 1].empty() &&
		    parts[i + 1].find_first_not_of("0123456789") == std::string::npos) {
			parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(i), parts.begin() + static_cast<std::ptrdiff_t>(i) + 2);
			break;
		}
	}

	std::string id;
	if (host == "drive.google.com") {
		if (parts.size() >= 3 && parts[0] == "file" && parts[1] == "d") {
			id = parts[2];
			out.writable = parts.size() >= 4;
		} else if (parts.size() == 1 && (parts[0] == "open" || parts[0] == "uc")) {
			id = QueryValue(query, "id");
		} else if (parts.size() == 3 && parts[0] == "drive" && parts[1] == "folders") {
			// Exactly the folder: "/folders/<id>/*.csv" is not a Drive address,
			// and silently dropping the "/*.csv" read the whole folder.
			id = parts[2];
			out.is_folder = true;
		} else if (parts.size() == 4 && parts[0] == "drive" && parts[1] == "mobile" && parts[2] == "folders") {
			id = parts[3];
			out.is_folder = true;
		}
	} else if (host == "docs.google.com") {
		static const char *const kTypes[] = {"document", "spreadsheets", "presentation", "forms", "drawings", "file"};
		for (auto type : kTypes) {
			if (parts.size() >= 3 && parts[0] == type && parts[1] == "d") {
				id = parts[2];
				out.is_editor = std::strcmp(type, "file") != 0;
			}
		}
		// A published form's /d/e/<key> is not a file id.
		if (id == "e") {
			id.clear();
		}
	} else if (host == "drive.usercontent.google.com") {
		if (parts.size() == 1 && parts[0] == "download") {
			id = QueryValue(query, "id");
		}
	}
	if (!IsId(id)) {
		out.error = "\"" + url +
		            "\" is a Google Drive address but names no file or folder. Use the address of a file "
		            "(drive.google.com/file/d/<id>/..., docs.google.com/spreadsheets/d/<id>/..., "
		            "drive.google.com/open?id=<id>) or a folder (drive.google.com/drive/folders/<id>).";
		out.is_folder = false;
		out.is_editor = false;
		out.writable = false;
		return out;
	}
	out.ok = true;
	out.id = id;
	out.version = QueryValue(query, "version");
	return out;
}

bool WebUrlToGDrivePath(const std::string &path, std::string &out, std::string &error) {
	if (!IsDriveWebUrl(path)) {
		out = path;
		return true;
	}
	auto parsed = ParseDriveWebUrl(path);
	if (!parsed.ok) {
		error = parsed.error;
		return false;
	}
	out = "gdrive://id:" + parsed.id;
	// Carried over, so the version reads -- and every write, move and remove
	// of it is refused. Dropped, remove_file(<address>?version=...) used to
	// remove the CURRENT file (found by exploratory testing).
	if (!parsed.version.empty()) {
		out += "?version=" + parsed.version;
	}
	return true;
}

} // namespace gdrive
} // namespace duckdb
