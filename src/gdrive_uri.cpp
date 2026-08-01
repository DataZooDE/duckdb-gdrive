#include "gdrive_uri.hpp"

#include <cstdint>
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

	// The "id:" form is special only in the LEADING position: a URI whose
	// path begins with "id:" addresses a single file, not a traversable
	// location, so it must be exactly "gdrive://id:<non-empty-id>" with no
	// further segments. "gdrive://id:1a2b3c/more" is rejected outright
	// rather than silently reinterpreted as an ordinary path whose first
	// segment happens to be "id:1a2b3c" -- a user who writes that has almost
	// certainly mistyped a file-id reference, and silently resolving
	// whatever folder happens to be named "id:1a2b3c" is a worse failure
	// mode than a clear, immediate error (codex review 2026-07-26, wave 0).
	//
	// A "id:"-prefixed segment deeper in the path (e.g. "gdrive://folder/
	// id:weird") is unaffected: only the leading position is special, so
	// that case falls through to the generic segment splitting below.
	if (rest.compare(0, 3, "id:") == 0) {
		if (rest.find('/') != std::string::npos) {
			result.ok = false;
			result.error = "the gdrive://id: form addresses a single file and cannot have path segments";
			return result;
		}
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

bool IsFileIdFallbackGlobProbe(const std::string &uri) {
	if (!IsGDriveUri(uri)) {
		return false;
	}
	const std::string rest = uri.substr(SCHEME_LEN);
	if (rest.compare(0, 3, "id:") != 0) {
		return false;
	}

	auto first_slash = rest.find('/');
	if (first_slash == std::string::npos) {
		return false;
	}
	if (first_slash == 3) {
		return false; // empty file id: "id:/..." -- not this shape either
	}

	// Exactly two more segments after the id, no more, no fewer.
	std::string remainder = rest.substr(first_slash + 1); // "**/*.<ext>" or garbage
	auto second_slash = remainder.find('/');
	if (second_slash == std::string::npos) {
		return false;
	}
	std::string first_seg = remainder.substr(0, second_slash);
	std::string second_seg = remainder.substr(second_slash + 1);
	if (first_seg != "**") {
		return false;
	}
	if (second_seg.empty() || second_seg.find('/') != std::string::npos) {
		return false; // must be exactly one trailing segment, nothing deeper
	}
	// "*.<ext>", extension non-empty (JoinPath never produces a bare "*.").
	return second_seg.size() > 2 && second_seg[0] == '*' && second_seg[1] == '.';
}

namespace {

std::string TrimAscii(const std::string &s) {
	const char *ws = " \t\r\n";
	auto first = s.find_first_not_of(ws);
	if (first == std::string::npos) {
		return "";
	}
	return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

} // namespace

bool PathMatchesAnyPrefix(const std::string &path, const std::string &csv_prefixes) {
	if (csv_prefixes.empty() || path.empty()) {
		return false;
	}
	size_t pos = 0;
	while (pos <= csv_prefixes.size()) {
		auto comma = csv_prefixes.find(',', pos);
		auto end = (comma == std::string::npos) ? csv_prefixes.size() : comma;
		std::string prefix = TrimAscii(csv_prefixes.substr(pos, end - pos));
		pos = end + 1;

		// A trailing slash is presentation, not meaning: "lake/" and "lake"
		// name the same directory, and requiring the user to guess which one
		// we want is a setting that silently does nothing when guessed wrong.
		while (prefix.size() > 1 && prefix.back() == '/') {
			prefix.pop_back();
		}
		if (prefix.empty()) {
			continue;
		}
		if (path == prefix) {
			return true;
		}
		// Segment boundary required -- see the header's note on why a bare
		// starts_with would let "lakehouse" inherit "lake"'s exemption.
		if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
		    path[prefix.size()] == '/') {
			return true;
		}
		if (comma == std::string::npos) {
			break;
		}
	}
	return false;
}

} // namespace gdrive
} // namespace duckdb
