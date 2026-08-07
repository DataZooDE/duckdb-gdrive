#pragma once

#include "datazoo_banner_duckdb.hpp"

// Shared identity for the load banner and the issue-link error footer.
//
// This extension's surface is a FileSystem, not registered function pointers,
// so DATAZOO_GUARD (which wraps a free function into an identical pointer) does
// not apply. The user-facing GDriveFileSystem methods use DATAZOO_GUARDED_BLOCK
// around their bodies instead.
//
// Defined in gdrive_extension.cpp.
extern const datazoo::BannerInfo GDRIVE_BANNER;
