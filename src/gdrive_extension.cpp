#define DUCKDB_EXTENSION_MAIN

#include "gdrive_extension.hpp"
#include "gdrive_filesystem.hpp"
#include "gdrive_stats.hpp"
#include "gdrive_version.hpp"

#ifndef EMSCRIPTEN
#include "telemetry.hpp"
#endif

#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// RegisterGDriveSecrets: the `gdrive` secret type + its providers (T1.A,
// src/gdrive_secret.cpp -- not this track's file set). That track has not
// published a header for it, so it is forward-declared here rather than
// guessed at through an #include this track does not own; the signature
// matches the call site the task brief fixes.
//
// RegisterGDriveStats comes from gdrive_stats.hpp (S-4.1, src/gdrive_stats.cpp
// + src/gdrive_client.cpp), which DOES exist and is included directly above.
// ---------------------------------------------------------------------------
namespace gdrive {
void RegisterGDriveSecrets(ExtensionLoader &loader);
} // namespace gdrive

static void GdriveVersionScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto version = gdrive::GdriveVersion();
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version);
}

static void LoadInternal(ExtensionLoader &loader) {
#ifndef EMSCRIPTEN
	// Anonymous usage telemetry -- same key, library and opt-out paths as
	// ../erpl, ../erpl-web, ../quack-oauth.
	PostHogTelemetry::Instance().SetAPIKey("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t");
	PostHogTelemetry::Instance().SetProduct("gdrive", gdrive::GdriveVersion(), "oss");
	PostHogTelemetry::Instance().AssociateGroup("deployment", PostHogTelemetry::GetDistinctId());
	PostHogTelemetry::Instance().CaptureExtensionLoad("gdrive", gdrive::GdriveVersion());
#endif

	ScalarFunction version_fn("gdrive_version", {}, LogicalType::VARCHAR, GdriveVersionScalar);
	loader.RegisterFunction(version_fn);

	// The extension point (HLD section 2): everything layered on DuckDB's
	// filesystem -- read_parquet, read_csv, COPY, glob, ATTACH -- inherits
	// gdrive:// from here without knowing it exists. Registered once; the
	// filesystem object spans every ClientContext for the life of the
	// DatabaseInstance (see the CacheKey security note in
	// gdrive_filesystem.hpp for why that matters).
	loader.GetDatabaseInstance().GetFileSystem().RegisterSubSystem(make_uniq<gdrive::GDriveFileSystem>());

	// D-6: RemoveFile trashes by default; this opts into permanent
	// files.delete. Trash is recoverable, which is the safer default on a
	// user's own Drive -- a table-format cleanup routine deleting the wrong
	// thing permanently is not.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "gdrive_permanent_delete",
	    "RemoveFile permanently deletes instead of moving to trash (default: false, trash).",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// D-7: Docs export to text/plain by default; text/markdown is not
	// byte-stable across exports, which would make GetVersionTag-keyed
	// caching lie. Sheets always export to text/csv regardless of this
	// setting.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "gdrive_docs_export_mime",
	    "MIME type to export application/vnd.google-apps.document files as: "
	    "'text/plain' (default) or 'text/markdown'. Sheets always export to text/csv.",
	    LogicalType::VARCHAR, Value("text/plain"));

	// REQ-NF-01. Drive's media endpoint costs ~1.2 s per request REGARDLESS
	// of size -- a 1 KB read and a 1 MB read measure the same, and the whole
	// 87 MB benchmark file in one request takes 2.06 s. So on Drive the
	// winning move is the opposite of the usual one: fetch MORE per request,
	// in FEWER requests, and share the result between threads.
	//
	// 0 disables the cache and restores exact ranged reads (lower memory,
	// far more requests).
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "gdrive_block_size_bytes",
	    "Block size for cached reads (default 16 MiB, 0 to disable and read exact ranges). "
	    "Drive charges roughly the same for a 1 KB and a 16 MB request, so larger blocks "
	    "trade bandwidth for far fewer round trips.",
	    LogicalType::UBIGINT, Value::UBIGINT(16ULL * 1024 * 1024));

	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "gdrive_block_cache_bytes",
	    "Total memory the shared block cache may hold (default 256 MiB). Least-recently-used "
	    "blocks are evicted above this. Shared by all files and all threads.",
	    LogicalType::UBIGINT, Value::UBIGINT(256ULL * 1024 * 1024));

	// S-2.11. The path->id map had no bound at all: entries were removed only
	// by explicit invalidation, so a long-lived process globbing many folders
	// grew it forever. 0 restores that unbounded behaviour deliberately.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "gdrive_path_cache_entries",
	    "Maximum path->file-id mappings to cache (default 4096, 0 for unbounded). "
	    "Least-recently-used entries are dropped above this; a dropped mapping just "
	    "costs one files.list per segment to rebuild.",
	    LogicalType::UBIGINT, Value::UBIGINT(4096));

	gdrive::RegisterGDriveSecrets(loader);
	gdrive::RegisterGDriveStats(loader);
}

void GdriveExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

string GdriveExtension::Name() {
	return "gdrive";
}

string GdriveExtension::Version() const {
	return gdrive::GdriveVersion();
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(gdrive, loader) {
	duckdb::LoadInternal(loader);
}
}
