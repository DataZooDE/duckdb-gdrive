#pragma once

#include <string>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Browser addresses as paths. PURE.
//
// What people paste is the address bar or a "Copy link" -- not a gdrive://
// path. Every such address names one file or folder by its Drive id, so it
// maps exactly onto the gdrive://id: form and costs no resolution at all:
//
//   https://drive.google.com/file/d/<id>/view?usp=sharing
//   https://drive.google.com/open?id=<id>          (and /uc?id=<id>)
//   https://drive.usercontent.google.com/download?id=<id>
//   https://docs.google.com/spreadsheets/d/<id>/edit#gid=0
//   https://docs.google.com/document/d/<id>/edit   (presentation, forms, drawings)
//   https://drive.google.com/drive/folders/<id>    (also /drive/u/1/folders/<id>)
//
// with an optional "/u/<n>" account selector and a legacy Workspace
// "/a/<domain>" prefix. The sibling duckdb-sharepoint does the same for
// SharePoint addresses; there a URL carries a path, here only an id.
// ---------------------------------------------------------------------------

//! Cheap prefix test for FileSystem::CanHandleFile: an http(s) address on
//! one of Drive's hosts. Must not allocate. A claimed address that then does
//! not parse is an error naming the forms that work, not someone else's file.
bool IsDriveWebUrl(const std::string &url);

struct DriveWebUrl {
	bool ok = false;
	std::string error;
	std::string id;
	//! A folder address (/drive/folders/<id>).
	bool is_folder = false;
	//! A Docs / Sheets / Slides / Forms / Drawings editor address: the file
	//! is a native Google one, readable through export, never writable.
	bool is_editor = false;
	//! drive.google.com/file/d/<id>/<more>: the one form a write may use.
	//! COPY stages to "tmp_" + the last segment, which here is not the id --
	//! a bare /file/d/<id> or open?id=<id> would stage onto another id or none.
	bool writable = false;
	//! A "version=<revision id>" in the query: the address names that version.
	std::string version;
};

DriveWebUrl ParseDriveWebUrl(const std::string &url);

//! The gdrive:// path an address denotes ("gdrive://id:<id>", with
//! "?version=<id>" when the address carries one), or `path`
//! itself when it is not a Drive address. False, with `error`, for an address
//! on a Drive host that names no file.
bool WebUrlToGDrivePath(const std::string &path, std::string &out, std::string &error);

} // namespace gdrive
} // namespace duckdb
