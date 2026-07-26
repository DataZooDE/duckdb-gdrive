// S-1.1 / S-1.2 -- the `gdrive` secret type.
//
// Registers TYPE gdrive with two creation providers, per HLD section 5.2:
//
//   PROVIDER config           -- paste pre-obtained tokens (ACCESS_TOKEN,
//                                 optionally REFRESH_TOKEN/CLIENT_ID/
//                                 CLIENT_SECRET for a future refresh path).
//   PROVIDER service_account  -- unattended server-to-server auth via a
//                                 service-account JSON key (KEY_FILE).
//
// `authorization_code` (the interactive browser flow) is NOT registered here.
// It needs datazoo-oauth2's OAuth2Flow/OAuth2Server/OAuth2Browser, which is
// not yet wired into this repo's CMake (plan D-2) -- see the report handed
// back with this slice for exactly what that provider still needs.
//
// REQ-NF-03 (redaction): every field that can carry secret material --
// access_token, refresh_token, client_secret -- is added to `redact_keys`,
// so `SELECT * FROM duckdb_secrets()` never displays it. KEY_FILE is a path,
// not key material, so it is deliberately NOT redacted -- but its *contents*
// are never copied into the secret_map or into any error message (S-1.8).
#include "gdrive_service_account.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <fstream>
#include <sstream>

namespace duckdb {
namespace gdrive {

namespace {

constexpr const char *kSecretTypeName = "gdrive";

// ---------------------------------------------------------------------------
// Reads the service-account key file for up-front validation at CREATE
// SECRET time, so a bad KEY_FILE is caught immediately rather than on the
// first Drive call (which would need network + a live secret to provoke).
//
// REQ-NF-03 / S-1.8: the file's contents are used only to call
// ParseServiceAccountKey (whose error messages are guaranteed key-material
// free) and are never themselves interpolated into an exception message.
// ---------------------------------------------------------------------------
std::string ReadKeyFileOrThrow(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f.good()) {
		throw InvalidInputException(
		    "gdrive: service-account key file not found or not readable: '%s'. "
		    "Check the KEY_FILE path passed to CREATE SECRET.",
		    path.c_str());
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void CopyOption(CreateSecretInput &input, KeyValueSecret &result, const char *key) {
	auto it = input.options.find(key);
	if (it != input.options.end()) {
		result.secret_map[key] = it->second;
	}
}

bool HasNonEmptyOption(const CreateSecretInput &input, const char *key) {
	auto it = input.options.find(key);
	return it != input.options.end() && !it->second.ToString().empty();
}

void ApplyDefaultScope(KeyValueSecret &result) {
	// REQ-NF-04: default to the narrowest scope when unspecified.
	if (result.secret_map.find("scope") == result.secret_map.end()) {
		result.secret_map["scope"] = Value(std::string(SCOPE_DRIVE_READONLY));
	}
}

// ---------------------------------------------------------------------------
// PROVIDER config -- pre-obtained tokens, pasted in.
// ---------------------------------------------------------------------------
unique_ptr<BaseSecret> CreateConfigSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	CopyOption(input, *result, "access_token");
	CopyOption(input, *result, "refresh_token");
	CopyOption(input, *result, "client_id");
	CopyOption(input, *result, "client_secret");
	CopyOption(input, *result, "drive_id");
	CopyOption(input, *result, "root_folder_id");
	CopyOption(input, *result, "scope");

	if (!HasNonEmptyOption(input, "access_token") && !HasNonEmptyOption(input, "refresh_token")) {
		throw InvalidInputException(
		    "gdrive secret (PROVIDER config) needs at least ACCESS_TOKEN or REFRESH_TOKEN. Example:\n"
		    "  CREATE SECRET gdrive_tok (TYPE gdrive, PROVIDER config, ACCESS_TOKEN '...');");
	}

	ApplyDefaultScope(*result);

	result->redact_keys.insert("access_token");
	result->redact_keys.insert("refresh_token");
	result->redact_keys.insert("client_secret");

	return std::move(result);
}

// ---------------------------------------------------------------------------
// PROVIDER service_account -- unattended auth via a service-account key file.
// ---------------------------------------------------------------------------
unique_ptr<BaseSecret> CreateServiceAccountSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	CopyOption(input, *result, "key_file");
	CopyOption(input, *result, "drive_id");
	CopyOption(input, *result, "root_folder_id");
	CopyOption(input, *result, "scope");

	if (!HasNonEmptyOption(input, "key_file")) {
		throw InvalidInputException(
		    "gdrive secret (PROVIDER service_account) needs KEY_FILE. Example:\n"
		    "  CREATE SECRET gdrive_sa (TYPE gdrive, PROVIDER service_account, "
		    "KEY_FILE '/etc/creds/sa.json');");
	}

	// S-1.8: validate the key file up front, with a distinct message per
	// failure mode. The parsed key material itself is discarded immediately
	// after validation -- only the KEY_FILE *path* is stored in the secret.
	const std::string key_file = input.options.find("key_file")->second.ToString();
	std::string key_json = ReadKeyFileOrThrow(key_file);
	auto parsed = ParseServiceAccountKey(key_json);
	key_json.clear();
	if (!parsed.ok) {
		throw InvalidInputException("gdrive: invalid service-account key file '%s': %s", key_file.c_str(),
		                             parsed.error.c_str());
	}

	ApplyDefaultScope(*result);

	// No secret material lives in this secret's map at all -- KEY_FILE is a
	// path. Nothing to redact for this provider, but the set exists for
	// forward-compatibility (e.g. if a future slice inlines the key).
	result->redact_keys.insert("private_key");

	return std::move(result);
}

void RegisterCommonParameters(CreateSecretFunction &function) {
	function.named_parameters["drive_id"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["root_folder_id"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["scope"] = LogicalType(LogicalTypeId::VARCHAR);
}

} // namespace

//! Registers the `gdrive` secret type and its `config` / `service_account`
//! creation providers. Called from the extension's Load() -- see the report
//! handed back with this slice for the exact call site expected in
//! src/gdrive_extension.cpp (a different track's file).
void RegisterGDriveSecrets(ExtensionLoader &loader) {
	SecretType type;
	type.name = kSecretTypeName;
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = "config";
	loader.RegisterSecretType(type);

	CreateSecretFunction config_fn = {kSecretTypeName, "config", CreateConfigSecret, {}};
	config_fn.named_parameters["access_token"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["refresh_token"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["client_secret"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(config_fn);
	loader.RegisterFunction(config_fn);

	CreateSecretFunction sa_fn = {kSecretTypeName, "service_account", CreateServiceAccountSecret, {}};
	sa_fn.named_parameters["key_file"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(sa_fn);
	loader.RegisterFunction(sa_fn);
}

} // namespace gdrive
} // namespace duckdb
