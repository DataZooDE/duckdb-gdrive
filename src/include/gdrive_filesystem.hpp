#pragma once

#include "gdrive_client.hpp"
#include "gdrive_uri.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <cstdint>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// GDriveFileSystem -- the extension point (HLD section 2).
//
// DuckDB routes every file access through a virtual filesystem, so everything
// layered on it inherits gdrive:// without knowing it exists: read_parquet,
// read_csv, COPY, glob, ATTACH, and table formats such as DuckLake whose data
// files and cleanup routines go through the same interface. That inheritance
// is the whole reason to implement here rather than in any one consumer.
//
// The obligation is larger than the happy path suggests -- see the method map
// in HLD section 3. Two mappings are deliberately hostile:
//
//   * Write(handle, buffer, n, location) throws. Drive cannot write at a byte
//     offset, and failing loudly beats corrupting silently (R-3).
//   * MoveFile is not atomic. DuckDB's storage manager assumes rename
//     atomicity; Drive does not provide it. Documented, not papered over.
// ---------------------------------------------------------------------------

//! Resolves gdrive:// paths to Drive file ids.
//!
//! This is the core problem (HLD section 4). Drive has NO path addressing, so
//! "a/b/c.parquet" costs one files.list per segment -- three round trips
//! before a single byte is read. That is risk R-1 and the most likely thing
//! to sink the project, so the cache is not an optimisation, it is the
//! mitigation.
//!
//! ---------------------------------------------------------------------
//! SECURITY: entries MUST be keyed by identity, not by path alone.
//!
//! The plan called this a "per-connection" cache, but the filesystem object
//! is registered once as a subsystem on the DatabaseInstance and therefore
//! outlives and spans every ClientContext. A cache keyed on the path string
//! alone would let connection A, authenticated with a read-only secret
//! against Shared Drive X, serve a resolved file id to connection B holding a
//! different secret and a different root -- a cross-tenant leak that looks
//! like a caching bug and reads like a permissions bug.
//!
//! CacheKey therefore includes the secret name, drive id and root folder, and
//! a hit is only honoured when the *current* auth context matches. Found in
//! codex review #1 (docs/reviews/2026-07-26-codex-review-1-wave0.md), before
//! any of it was implemented.
//! ---------------------------------------------------------------------
struct CacheKey {
	std::string secret_name;
	std::string drive_id;
	std::string root_folder_id;
	std::string canonical_path;

	bool operator==(const CacheKey &other) const;
	//! Stable string form, used as the map key.
	std::string ToString() const;
};

class GDrivePathCache {
public:
	//! Look up a fully-resolved path. Returns false on a miss.
	bool TryGet(const CacheKey &key, DriveFileMeta &out);
	void Put(const CacheKey &key, const DriveFileMeta &meta);
	//! Drop `key` and everything beneath it, within the SAME identity. A
	//! rename or delete invalidates every descendant path, but must not
	//! disturb another identity's entries.
	void InvalidatePrefix(const CacheKey &key);

	//! ---------------------------------------------------------------------
	//! Metadata by FILE ID, for one identity, scoped to ONE QUERY.
	//!
	//! Separate from the path map above because it answers a different
	//! question ("what are this id's size and headRevisionId?") and because
	//! its validity is bounded, which path->id mappings' is not.
	//!
	//! Scoped by DuckDB's active query number rather than a wall-clock TTL.
	//! A TTL was tried first and was wrong: it let a file deleted and
	//! recreated between two queries be read at its OLD size, because
	//! OpenFile's metadata refresh -- which is what detects a dead id -- was
	//! served from cache instead of hitting Drive. The e2e stale-cache test
	//! caught it. Query scoping gives the same saving (all of a scan's
	//! per-thread opens share one fetch) with no staleness across queries at
	//! all, and no dependence on timing.
	//!
	//! Why it exists: DuckDB's parallel Parquet scan opens one handle PER
	//! THREAD, and every OpenFile fetched metadata. Measured on a 32-thread
	//! box: 19 identical files.get calls for one query, ~150 ms each. The
	//! count scaled with `SET threads` (1->3, 4->6, 32->19) while the data
	//! requests stayed at 35, which is what identified this as the fixable
	//! half of REQ-NF-01's failure.
	//!
	//! Keyed by identity, exactly like the path map, and for the same reason:
	//! one FileSystem object serves every ClientContext, so a cache keyed by
	//! file id alone would serve one tenant's metadata to another. That is
	//! not hypothetical -- the token cache had precisely this bug.
	//! ---------------------------------------------------------------------
	//! Start of a query. Clears metadata if `generation` differs from the
	//! last one seen. Generation 0 means "no query context": nothing is
	//! cached or served, because an entry we cannot scope is one we cannot
	//! safely reuse.
	void BeginQuery(idx_t generation);
	bool TryGetMetadata(const CacheKey &identity, const std::string &file_id, DriveFileMeta &out);
	void PutMetadata(const CacheKey &identity, const std::string &file_id, const DriveFileMeta &meta);
	//! Drop everything belonging to one secret -- called when a secret is
	//! dropped or re-created, since its ids may no longer be reachable.
	void InvalidateSecret(const std::string &secret_name);
	void Clear();
	idx_t Size();

private:
	mutex lock;
	unordered_map<std::string, DriveFileMeta> entries;

	unordered_map<std::string, DriveFileMeta> metadata_entries;
	idx_t metadata_generation = 0;
};

//! Per-handle state. Metadata is captured once at OpenFile and reused: asking
//! Drive again for a size we already know is exactly the amplification REQ-NF-02
//! forbids.
class GDriveFileHandle : public FileHandle {
public:
	GDriveFileHandle(FileSystem &fs, string path, FileOpenFlags flags, DriveFileMeta meta);
	~GDriveFileHandle() override;

	void Close() override;

	DriveFileMeta meta;
	//! Sequential read/write cursor.
	idx_t position = 0;

	//! Native Google formats have no byte stream, so the whole export is
	//! fetched once and served from here. GetFileSize then reports the
	//! EXPORTED length -- Drive reports no `size` for such files at all.
	bool is_export = false;
	std::string export_buffer;

	//! Write path: bytes accumulate locally and nothing reaches Drive until
	//! Close(). Drive has no append and no positional write, so a buffered
	//! whole-file upload is the only honest mapping (HLD section 7).
	bool is_write = false;
	std::string write_buffer;
	std::string write_parent_id;
	std::string write_name;
	//! Set when a write failed, so Close() does NOT upload a truncated file.
	//! Uploading whatever happened to be buffered when an exception unwound
	//! would silently publish a corrupt file to the user's Drive.
	bool write_failed = false;
};

class GDriveFileSystem : public FileSystem {
public:
	GDriveFileSystem();
	~GDriveFileSystem() override;

	//! Cheap prefix test; DuckDB calls this on every path for every filesystem.
	bool CanHandleFile(const string &fpath) override;
	std::string GetName() const override {
		return "GDriveFileSystem";
	}

	//! NB: this is the *virtual* OpenFile in v1.5.3. The OpenFileInfo overload
	//! on the base class is non-virtual and delegates here; the richer
	//! OpenFileExtended hook is protected and opt-in via
	//! SupportsOpenFileExtended(). Overriding the wrong one compiles fine and
	//! is simply never called, which is a memorable afternoon.
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;

	//! The hot path. Issues GET files/{id}?alt=media with a Range header.
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	//! Sequential only -- buffers locally, uploads once on Close().
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	//! Returns headRevisionId. Core documents this as the cache-invalidation
	//! tag used by the HTTP filesystem's caching layer, and Drive's revision
	//! id fits that contract exactly -- so caching integrates properly rather
	//! than being bolted on.
	std::string GetVersionTag(FileHandle &handle) override;

	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool ListFiles(const string &directory,
	               const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr) override;
	void MoveFile(const string &source, const string &target,
	              optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;

	void FileSync(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}

	//! Drive cannot truncate or trim in place; both throw.
	void Truncate(FileHandle &handle, int64_t new_size) override;
	bool Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override;

private:
	//! Resolve a gdrive:// path to metadata, consulting the cache first.
	//!
	//! More than one match is an ERROR naming both file ids and suggesting
	//! the id: form (R-4). Drive permits duplicate names in one folder, so
	//! silently picking one would make query results depend on Drive's
	//! internal ordering -- a bug that reproduces only sometimes.
	DriveFileMeta ResolveOrThrow(ClientContext &context, const GDriveUri &uri);

	//! R-4 applies to LISTING too, not just resolution.
	//!
	//! Glob and ListFiles return paths, and two siblings sharing a name
	//! produce two identical paths -- indistinguishable to the caller, and
	//! ambiguous again when one is later opened. Deferring the error to open
	//! time means a scan can half-succeed.
	//!
	//! Rule: when a listing contains duplicate sibling names, emit the
	//! `gdrive://id:<fileId>` form for the affected entries so every returned
	//! path addresses exactly one file. Unambiguous entries keep their
	//! readable path form.
	static std::string DisambiguatePath(const std::string &parent_path,
	                                    const DriveFileMeta &meta, bool name_is_ambiguous);

	GDrivePathCache cache;

	//! Re-resolve a handle whose cached file id has gone dead. See the .cpp.
	//! Takes the base handle: GDriveFileHandleImpl lives in the private
	//! gdrive_internal.hpp, which this public contract must not depend on.
	bool TryRecoverStaleHandle(GDriveFileHandle &handle);
};

} // namespace gdrive
} // namespace duckdb
