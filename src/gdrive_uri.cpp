#include "gdrive_uri.hpp"

#include <cstring>

namespace duckdb {
namespace gdrive {

namespace {

constexpr size_t SCHEME_LEN = 9; // strlen("gdrive://")

//! Split the remainder-after-scheme into slash-separated pieces, collapsing
//! repeated slashes and ignoring a single trailing slash. Does not validate
//! segment contents -- that is the caller's job.
std::vector<std::string> SplitSegments(const std::string &rest) {
	std::vector<std::string> segments;
	size_t i = 0;
	const size_t n = rest.size();
	while (i < n) {
		// Skip any run of slashes.
		while (i < n && rest[i] == '/') {
			i++;
		}
		if (i >= n) {
			break;
		}
		size_t start = i;
		while (i < n && rest[i] != '/') {
			i++;
		}
		segments.emplace_back(rest.substr(start, i - start));
	}
	return segments;
}

} // namespace

bool IsGDriveUri(const std::string &uri) {
	if (uri.size() < SCHEME_LEN) {
		return false;
	}
	return std::memcmp(uri.data(), GDRIVE_SCHEME, SCHEME_LEN) == 0;
}

std::string GDriveUri::ToString() const {
	if (kind == GDriveUriKind::FILE_ID) {
		return std::string(GDRIVE_SCHEME) + "id:" + file_id;
	}
	std::string out(GDRIVE_SCHEME);
	for (size_t i = 0; i < segments.size(); i++) {
		if (i > 0) {
			out += "/";
		}
		out += segments[i];
	}
	return out;
}

std::string GDriveUri::ParentPath() const {
	if (kind != GDriveUriKind::PATH || segments.empty()) {
		return std::string(GDRIVE_SCHEME);
	}
	std::string out(GDRIVE_SCHEME);
	for (size_t i = 0; i + 1 < segments.size(); i++) {
		if (i > 0) {
			out += "/";
		}
		out += segments[i];
	}
	return out;
}

std::string GDriveUri::FileName() const {
	if (kind != GDriveUriKind::PATH || segments.empty()) {
		return std::string();
	}
	return segments.back();
}

GDriveUriParse ParseGDriveUri(const std::string &uri) {
	GDriveUriParse result;

	if (!IsGDriveUri(uri)) {
		result.ok = false;
		result.error = "not a gdrive:// URI: \"" + uri + "\"";
		return result;
	}

	const std::string rest = uri.substr(SCHEME_LEN);

	// The "id:" form is recognized only when it names the ENTIRE remaining
	// path, i.e. there is no slash-separated segment after it: a file id
	// addresses one file, not a traversable location. "gdrive://id:x/more"
	// is therefore an ordinary path whose first segment happens to start
	// with "id:", handled by the generic segment splitting below.
	if (rest.compare(0, 3, "id:") == 0 && rest.find('/') == std::string::npos) {
		std::string file_id = rest.substr(3);
		if (file_id.empty()) {
			result.ok = false;
			result.error = "gdrive:// id: form requires a non-empty file id, got \"" + uri + "\"";
			return result;
		}
		result.ok = true;
		result.uri.kind = GDriveUriKind::FILE_ID;
		result.uri.file_id = file_id;
		return result;
	}

	std::vector<std::string> segments = SplitSegments(rest);

	for (const auto &seg : segments) {
		if (seg == ".") {
			result.ok = false;
			result.error = "gdrive:// path segment \".\" is rejected: Drive has no concept of the "
			               "current directory (in \"" +
			               uri + "\")";
			return result;
		}
		if (seg == "..") {
			result.ok = false;
			result.error = "gdrive:// path segment \"..\" is rejected: Drive has no concept of parent "
			               "traversal (in \"" +
			               uri + "\")";
			return result;
		}
	}

	result.ok = true;
	result.uri.kind = GDriveUriKind::PATH;
	result.uri.segments = std::move(segments);
	return result;
}

} // namespace gdrive
} // namespace duckdb
