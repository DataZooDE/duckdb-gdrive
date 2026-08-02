#include "gdrive_vfs_functions.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

// int64_t is used below. libstdc++ leaks it transitively, musl's libc++ does
// not, and musl is in the 1.4 LTS build matrix -- scripts/check_cstdint.sh
// guards exactly this.
#include <cstdint>
#include <utility>

namespace duckdb {
namespace gdrive {

namespace {

// Why FileExists before RemoveFile, rather than catching the throw: callers
// need to tell "it was there and now it is gone" from "there was nothing to
// delete" WITHOUT that distinction arriving as an exception, because on a
// remote filesystem the not-found path is the common, expected one during a
// retry or a re-run. Returning a boolean keeps it a value, not control flow.
//
// This is a check-then-act and therefore racy by construction: a concurrent
// delete between the two calls surfaces as whatever the filesystem throws
// from RemoveFile. That race is not closable here -- neither DuckDB's
// FileSystem nor Drive's API offers a delete-if-exists -- so it is documented
// rather than papered over.
//
// NOTE: no FileOpener is passed to any of these calls. What
// FileSystem::GetFileSystem(context) hands back is an OpenerFileSystem, a
// wrapper that injects the context's own opener into every call it forwards
// -- and asserts (VerifyNoOpener) if the caller supplied one as well. Passing
// a ClientContextFileOpener here is not merely redundant, it aborts the query
// with "OpenerFileSystem cannot take an opener". Secret lookup still works:
// the opener the wrapper pushes is the one that resolves `gdrive` secrets.
//! Confirm that `path` is genuinely ABSENT rather than unreachable.
//!
//! `FileExists` returning false is not proof of absence. GDriveFileSystem
//! deliberately answers false when no secret is configured or the URI does not
//! parse -- it has to, because DuckDB core probes speculatively during
//! replacement-scan binding and would break if that threw. Building a
//! user-facing "does not exist" on top of that leniency reported a
//! CONFIGURATION error as a missing file: `remove_file` returned false and
//! `file_size` returned NULL for a file that was plainly there.
//!
//! Opening with NULL_IF_NOT_EXISTS is the strict question: null means the
//! filesystem looked and found nothing; anything else throws. Only reached on
//! the false path, so an existing file never pays for it.
bool ConfirmedAbsent(FileSystem &fs, const string &path) {
	auto probe = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
	return probe == nullptr;
}

bool RemoveOne(FileSystem &fs, const string &path) {
	if (!fs.FileExists(path)) {
		if (ConfirmedAbsent(fs, path)) {
			return false;
		}
		// Reachable, and not a file. Almost always a directory -- say so,
		// because "cannot be removed" without a reason sends people looking
		// for a permissions problem.
		if (fs.DirectoryExists(path)) {
			throw IOException("gdrive: '%s' is a directory, not a file; remove_file does not remove "
			                  "directories",
			                  path);
		}
		throw IOException("gdrive: '%s' exists but is not a removable file", path);
	}
	fs.RemoveFile(path);
	return true;
}

void RemoveFileScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());

	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(),
	                                       [&](string_t path) { return RemoveOne(fs, path.GetString()); });
}

void MoveFileScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());

	BinaryExecutor::Execute<string_t, string_t, bool>(args.data[0], args.data[1], result, args.size(),
	                                                  [&](string_t source, string_t target) {
		                                                  fs.MoveFile(source.GetString(), target.GetString());
		                                                  return true;
	                                                  });
}

// Create every missing directory above `target`, shallowest first.
//
// Deliberately NOT FileSystem::CreateDirectoriesRecursive. That walks upward
// with Path::Parent(), which does not understand a URL-form path: on
// `gdrive://a/b/c.md` it stops after one level, so writing into a
// two-deep-or-more new subtree fails with "no such file or directory:
// 'gdrive://a'" -- verified against live Drive. Walking DOWNWARD from the
// scheme is scheme-agnostic and creates each level in turn.
//
// OpenFile does not create parents either, so without this the first write
// into any new subdirectory fails -- which is every write into a fresh
// tenant, not a rare edge. It is also the only way to create a directory at
// all from SQL, CreateDirectory being equally unreachable.
void EnsureParentDirectories(FileSystem &fs, const string &target) {
	auto last_sep = target.find_last_of('/');
	if (last_sep == string::npos) {
		return;
	}
	// Skip past "scheme://" (or a leading "/") so the scheme's own slashes
	// are never mistaken for directory separators.
	size_t start = 0;
	auto scheme = target.find("://");
	if (scheme != string::npos) {
		start = scheme + 3;
	} else if (!target.empty() && target.front() == '/') {
		start = 1;
	}

	for (auto i = target.find('/', start); i != string::npos && i <= last_sep; i = target.find('/', i + 1)) {
		auto dir = target.substr(0, i);
		if (dir.empty()) {
			continue;
		}
		// DirectoryExists first: on a remote filesystem a redundant create is
		// a wasted round trip, and on Drive it can produce a DUPLICATE folder
		// of the same name rather than an error.
		if (!fs.DirectoryExists(dir)) {
			// ...but "not a directory" is not the same as "not there". If a
			// FILE already occupies this segment, creating a folder beside it
			// gives Drive two entries with one name -- an R-4 ambiguity
			// manufactured by us, which then makes the original file
			// unreadable. Every other filesystem answers ENOTDIR here.
			if (fs.FileExists(dir)) {
				throw IOException(
				    "gdrive: cannot create directory '%s': a file of that name already exists. "
				    "Writing under it would create a second entry with the same name, which Drive "
				    "allows and which would make both unaddressable by path.",
				    dir);
			}
			fs.CreateDirectory(dir);
		}
	}
}

// The inverse of core's read_blob(). FILE_CREATE_NEW rather than FILE_CREATE
// is what gives overwrite semantics -- FILE_CREATE opens an existing file
// without truncating, which would leave a tail of the previous, longer
// content behind and silently corrupt every shorter rewrite.
//
// Writes are issued as a single Write() call. That matters on gdrive://,
// whose handle only supports sequential append and uploads on close: one
// call is one upload, so the file appears complete or not at all.
void WriteBlobScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());

	BinaryExecutor::Execute<string_t, string_t, int64_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t path, string_t content) {
		    auto target = path.GetString();
		    EnsureParentDirectories(fs, target);
		    auto handle = fs.OpenFile(target, FileFlags::FILE_FLAGS_WRITE |
		                                          FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		    if (!handle) {
			    throw IOException("write_blob: could not open '%s' for writing", target);
		    }
		    auto size = NumericCast<int64_t>(content.GetSize());
		    if (size > 0) {
			    // const_cast: FileSystem::Write takes void*, not const void*,
			    // though it does not modify the buffer. string_t's data is
			    // owned by the input vector and outlives this call.
			    handle->Write(const_cast<char *>(content.GetDataUnsafe()), size);
		    }
		    handle->Sync();
		    handle->Close();
		    return size;
	    });
}

// Size WITHOUT transferring the body. read_blob() would report the same
// number, but only after downloading every byte -- on a remote filesystem
// that is the difference between one metadata call and a full download.
void FileSizeScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());

	UnaryExecutor::ExecuteWithNulls<string_t, int64_t>(
	    args.data[0], result, args.size(), [&](string_t path, ValidityMask &mask, idx_t idx) -> int64_t {
		    auto p = path.GetString();
		    // NULL for absent, mirroring remove_file's false: "no such file"
		    // is an expected answer on a remote store, not an exception. But
		    // ONLY for genuine absence -- see ConfirmedAbsent.
		    if (!fs.FileExists(p)) {
			    if (ConfirmedAbsent(fs, p)) {
				    mask.SetInvalid(idx);
				    return 0;
			    }
			    if (fs.DirectoryExists(p)) {
				    throw IOException("gdrive: '%s' is a directory, not a file; it has no byte size", p);
			    }
			    throw IOException("gdrive: '%s' exists but its size cannot be read as a file", p);
		    }
		    auto handle = fs.OpenFile(p, FileFlags::FILE_FLAGS_READ);
		    if (!handle) {
			    mask.SetInvalid(idx);
			    return 0;
		    }
		    return NumericCast<int64_t>(handle->GetFileSize());
	    });
}

} // namespace

void RegisterVfsFunctions(ExtensionLoader &loader) {
	{
		ScalarFunction fn("remove_file", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, RemoveFileScalar);
		// A delete is not a pure function of its argument: the same call twice
		// returns true then false. Marking it volatile keeps the optimiser from
		// folding it to a constant or hoisting it out of a projection.
		fn.stability = FunctionStability::VOLATILE;
		CreateScalarFunctionInfo info(fn);

		FunctionDescription desc;
		desc.description =
		    "Delete the file at `path`, returning true if it existed and was removed and false if it did not "
		    "exist. Dispatches on the path's scheme through DuckDB's virtual filesystem, so it works for "
		    "gdrive://, s3://, gs:// and local paths alike. For a gdrive:// path the file is moved to the "
		    "trash unless gdrive_permanent_delete is set. Errors other than not-found are raised, not "
		    "returned as false.";
		desc.parameter_names = {"path"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT remove_file('gdrive://reports/old.parquet')"};
		desc.categories = {"gdrive"};
		info.descriptions.push_back(std::move(desc));

		loader.RegisterFunction(std::move(info));
	}

	ScalarFunction fn("move_file", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                  MoveFileScalar);
	fn.stability = FunctionStability::VOLATILE;
	CreateScalarFunctionInfo info(fn);

	FunctionDescription desc;
	desc.description =
	    "Rename/move `source` to `target`, returning true on success and raising on failure. Dispatches on "
	    "the path's scheme through DuckDB's virtual filesystem. Both paths must live on the SAME filesystem "
	    "-- this is a rename, not a copy, and it does not move bytes between schemes. Its main use is "
	    "publishing a fully-written temporary file under its final name.";
	desc.parameter_names = {"source", "target"};
	desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	desc.examples = {"SELECT move_file('gdrive://staging/part.tmp', 'gdrive://data/part.parquet')"};
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));

	loader.RegisterFunction(std::move(info));

	{
		ScalarFunction write_fn("write_blob", {LogicalType::VARCHAR, LogicalType::BLOB}, LogicalType::BIGINT,
		                        WriteBlobScalar);
		write_fn.stability = FunctionStability::VOLATILE;
		CreateScalarFunctionInfo write_info(write_fn);

		FunctionDescription write_desc;
		write_desc.description =
		    "Write `content` to `path`, replacing any existing file, and return the number of bytes "
		    "written. The inverse of read_blob(): together they give SQL a byte-exact round trip for any "
		    "filesystem DuckDB can reach, including gdrive://, with no CSV/Parquet encoding in between. "
		    "Accepts arbitrary binary content -- a BLOB, not a VARCHAR -- so it is safe for images and "
		    "PDFs as well as text.";
		write_desc.parameter_names = {"path", "content"};
		write_desc.parameter_types = {LogicalType::VARCHAR, LogicalType::BLOB};
		write_desc.examples = {"SELECT write_blob('gdrive://notes/readme.md', '# Title'::BLOB)"};
		write_desc.categories = {"gdrive"};
		write_info.descriptions.push_back(std::move(write_desc));

		loader.RegisterFunction(std::move(write_info));
	}

	ScalarFunction size_fn("file_size", {LogicalType::VARCHAR}, LogicalType::BIGINT, FileSizeScalar);
	size_fn.stability = FunctionStability::VOLATILE;
	CreateScalarFunctionInfo size_info(size_fn);

	FunctionDescription size_desc;
	size_desc.description =
	    "Byte length of the file at `path`, or NULL if it does not exist. Reads only metadata for ordinary "
	    "files -- unlike read_blob, which downloads the body. NOTE: a native Google Doc or Sheet has no "
	    "stored byte size, so its size is the length of the EXPORT (see gdrive_docs_export_mime) and "
	    "obtaining it downloads that export. Unlike "
	    "read_blob(), which reports the same number but downloads the whole body to do it.";
	size_desc.parameter_names = {"path"};
	size_desc.parameter_types = {LogicalType::VARCHAR};
	size_desc.examples = {"SELECT file_size('gdrive://data/part.parquet')"};
	size_desc.categories = {"gdrive"};
	size_info.descriptions.push_back(std::move(size_desc));

	loader.RegisterFunction(std::move(size_info));
}

} // namespace gdrive
} // namespace duckdb
