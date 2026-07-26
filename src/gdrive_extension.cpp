#define DUCKDB_EXTENSION_MAIN

#include "gdrive_extension.hpp"
#include "gdrive_version.hpp"

#ifndef EMSCRIPTEN
#include "telemetry.hpp"
#endif

#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

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
