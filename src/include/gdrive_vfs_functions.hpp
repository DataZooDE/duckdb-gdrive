#pragma once

// ---------------------------------------------------------------------------
// Scheme-generic file-mutation SQL functions.
//
// DuckDB's SQL surface can READ a file's bytes from any registered filesystem
// (`read_blob`, `read_text`) and ENUMERATE it (`glob`), and the C API exposes
// open/read/write/seek/size on a file handle. What neither offers is a way to
// DELETE or RENAME a file. `duckdb::FileSystem` has had `RemoveFile` and
// `MoveFile` all along -- they are simply not reachable from SQL, and not
// reachable from the C API either.
//
// These two functions close that gap. They are deliberately NOT named
// `gdrive_*`: they resolve through `FileSystem::GetFileSystem(context)`, the
// VIRTUAL filesystem, so they dispatch on the path's scheme and work for
// `gdrive://`, `s3://`, `gs://`, `hf://` and plain local paths alike. Any
// filesystem registered by any extension is covered without further work.
//
// They live in this extension because it is the one that needs them first
// (an escurel LaneStore backed by the DuckDB VFS, which needs delete to
// implement its storage contract). Nothing here depends on Drive; if a
// standalone VFS extension ever appears, this file moves there unchanged.
//
// Both honour the same `gdrive_permanent_delete` setting the filesystem does
// -- for a `gdrive://` path that decides trash vs. permanent delete, and for
// every other scheme it is simply ignored by the underlying filesystem.
// ---------------------------------------------------------------------------

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! Registers `remove_file(path)` and `move_file(source, target)`. Call once
//! from gdrive_extension.cpp's LoadInternal().
void RegisterVfsFunctions(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
