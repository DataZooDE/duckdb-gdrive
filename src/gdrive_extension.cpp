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
