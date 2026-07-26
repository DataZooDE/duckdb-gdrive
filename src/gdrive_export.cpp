// S-3.1..S-3.4 -- native Google formats (REQ-F-07, D-7).
//
// application/vnd.google-apps.* files (Sheets, Docs, ...) have no byte
// stream at all; alt=media fails on them with NOT_DOWNLOADABLE. files.export
// is the only way to read their content, and it does not support Range, so
// the whole export is fetched once and cached on the handle (see
// GDriveFileSystem::OpenFile in gdrive_filesystem.cpp, which sets is_export/
// export_buffer and reports GetFileSize as the EXPORTED length -- Drive
// reports no `size` for native files at all).
#include "gdrive_internal.hpp"

namespace duckdb {
namespace gdrive {

namespace {
constexpr const char *kSheetMimeType = "application/vnd.google-apps.spreadsheet";
constexpr const char *kDocMimeType = "application/vnd.google-apps.document";
} // namespace

std::string ExportMimeTypeFor(const DriveFileMeta &meta, const std::string &docs_export_mime_setting) {
	if (meta.mime_type == kSheetMimeType) {
		// D-7: Sheets always export to CSV. There is no "markdown
		// spreadsheet" to opt into, so the setting does not apply here.
		return "text/csv";
	}
	if (meta.mime_type == kDocMimeType) {
		// D-7: text/plain by default. text/markdown is an explicit opt-in
		// (gdrive_docs_export_mime) because markdown is not byte-stable
		// across exports, which would make GetVersionTag-keyed caching lie.
		if (docs_export_mime_setting == "text/markdown") {
			return "text/markdown";
		}
		return "text/plain";
	}
	// Other native formats (Slides, Drawings, Forms, ...) have no requirement
	// in scope yet; fall back to a plain-text export. Drive rejects
	// unsupported (source, target) pairs with its own error, which
	// ThrowGDriveError surfaces distinctly rather than us guessing further.
	return "text/plain";
}

std::string FetchExport(GDriveClient &client, const DriveFileMeta &meta, const std::string &export_mime_type,
                        const std::string &context_path) {
	auto resp = client.Export(meta.id, export_mime_type);
	if (!resp.ok) {
		ThrowGDriveError(resp.error, context_path);
	}
	return resp.body;
}

} // namespace gdrive
} // namespace duckdb
