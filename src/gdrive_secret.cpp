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
	//
	// BUG FIX (live run 2026-07-26): this option is deliberately named
	// "drive_scope", NOT "scope". DuckDB's CREATE SECRET grammar hard-codes
	// "SCOPE" as a top-level clause (see transform_create_secret.cpp,
	// PEGTransformerFactory::TransformCreateSecretStmt): any option literally
	// named `scope` is diverted into CreateSecretInfo::scope -- the secret's
	// PATH-matching prefix list used by SecretManager::LookupSecret -- and
	// never reaches `input.options` at all. A user who wrote
	// `CREATE SECRET g (TYPE gdrive, ..., SCOPE 'https://www.googleapis.com/
	// auth/drive')` intending to request a wider-than-default OAuth scope
	// instead silently rescoped the secret to only match paths starting with
	// that literal URL string -- so it never matches any `gdrive://` path,
	// LookupSecret reports no match, and every Drive call fails before a
	// single HTTP request is made. Naming this option `drive_scope` sidesteps
	// the collision entirely; "SCOPE '...'" continues to mean what it means
	// for every other DuckDB secret type (a path-prefix restriction), which
	// gdrive secrets keep the ability to use too, just not for the OAuth
	// scope string.
	if (result.secret_map.find("drive_scope") == result.secret_map.end()) {
		result.secret_map["drive_scope"] = Value(std::string(SCOPE_DRIVE_READONLY));
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
	CopyOption(input, *result, "drive_scope");

	if (!HasNonEmptyOption(input, "access_token") && !HasNonEmptyOption(input, "refresh_token")) {
		throw InvalidInputException(
		    "gdrive secret (PROVIDER config) needs at least ACCESS_TOKEN or REFRESH_TOKEN. Example:\n"
		    "  CREATE SECRET gdrive_tok (TYPE gdrive, PROVIDER config, ACCESS_TOKEN '...');");
	}

	// A bare ACCESS_TOKEN needs nothing else -- it is used as-is (pass-
	// through), never refreshed. But a REFRESH_TOKEN with no ACCESS_TOKEN is
	// useless without CLIENT_ID and CLIENT_SECRET to mint a fresh access
	// token from it (Google's grant_type=refresh_token POST needs all
	// three). Catching this at CREATE SECRET time, rather than on the first
	// gdrive:// access, needs no network call and fails loudly at the point
	// the user made the mistake.
	if (!HasNonEmptyOption(input, "access_token") &&
	    (!HasNonEmptyOption(input, "client_id") || !HasNonEmptyOption(input, "client_secret"))) {
		throw InvalidInputException(
		    "gdrive secret (PROVIDER config) has REFRESH_TOKEN but no ACCESS_TOKEN; refreshing needs CLIENT_ID and "
		    "CLIENT_SECRET too (Google's token endpoint needs all three). Example:\n"
		    "  CREATE SECRET gdrive_tok (TYPE gdrive, PROVIDER config, CLIENT_ID '...', CLIENT_SECRET '...', "
		    "REFRESH_TOKEN '...');");
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
	CopyOption(input, *result, "drive_scope");

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

// ---------------------------------------------------------------------------
// PROVIDER credential_chain -- Application Default Credentials (D-10).
//
// Takes no credential arguments at all, by design: a user who has already run
// `gcloud auth application-default login` should type one statement and
// nothing else. The secret is a MARKER -- discovery happens on first use, in
// GetAuthContext, so that rotating the underlying credential (or logging in
// after the secret was created) does not require recreating it.
//
// Nothing is validated here for the same reason. A CREATE SECRET that
// succeeded and then failed on first read would be a worse experience than
// either extreme, so the not-found message (NoCredentialsMessage) is the
// single place this is reported, and it teaches the fix.
// ---------------------------------------------------------------------------
unique_ptr<BaseSecret> CreateCredentialChainSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	// Where the identity comes from is this provider's business; which Drive
	// it addresses is not, so the root binding options still apply.
	CopyOption(input, *result, "drive_id");
	CopyOption(input, *result, "root_folder_id");
	CopyOption(input, *result, "drive_scope");

	ApplyDefaultScope(*result);

	// No credential material is stored -- only a path is ever resolved, and
	// only at use time. Nothing to redact.
	return std::move(result);
}

// ---------------------------------------------------------------------------
// PROVIDER authorization_code -- interactive browser consent (C-1).
//
// D-9: you bring your own OAuth client. No client id is compiled into this
// extension, deliberately. Embedding one is what lets the gsheets extension
// offer `CREATE SECRET (TYPE gsheet);` with no arguments, and it costs a
// Google verification track (Drive scopes are "restricted", so full
// verification means a CASA security assessment), a 100-test-user cap until
// that completes, and every user's consent screen naming us. The zero-setup
// path here is PROVIDER credential_chain instead, which reuses the `gcloud`
// login a developer already has.
//
// Creation stores configuration ONLY -- no browser is launched here. The flow
// runs on first use, in GetAuthContext. Two reasons: CREATE SECRET that
// blocks on a human clicking a consent screen is a surprising thing for a SQL
// statement to do, and doing it lazily is what makes creation testable
// without a browser. This matches erpl-web's datasphere secret.
// ---------------------------------------------------------------------------
unique_ptr<BaseSecret> CreateAuthorizationCodeSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	CopyOption(input, *result, "client_id");
	CopyOption(input, *result, "client_secret");
	CopyOption(input, *result, "redirect_port");
	CopyOption(input, *result, "drive_id");
	CopyOption(input, *result, "root_folder_id");
	CopyOption(input, *result, "drive_scope");

	// Named individually so the reader is told WHICH half is missing. Google's
	// token endpoint needs both, and finding that out on first query instead
	// of here would mean a browser consent completing and then failing.
	if (!HasNonEmptyOption(input, "client_id")) {
		throw InvalidInputException(
		    "gdrive secret (PROVIDER authorization_code) needs CLIENT_ID. Create an OAuth client of type "
		    "'Desktop app' in the Google Cloud console, then:\n"
		    "  CREATE SECRET gdrive_user (TYPE gdrive, PROVIDER authorization_code, CLIENT_ID '...', "
		    "CLIENT_SECRET '...');\n"
		    "If you would rather not create one, PROVIDER credential_chain uses your existing gcloud login.");
	}
	if (!HasNonEmptyOption(input, "client_secret")) {
		throw InvalidInputException(
		    "gdrive secret (PROVIDER authorization_code) needs CLIENT_SECRET as well as CLIENT_ID. Google's token "
		    "endpoint requires both, even for a Desktop-app client. Example:\n"
		    "  CREATE SECRET gdrive_user (TYPE gdrive, PROVIDER authorization_code, CLIENT_ID '...', "
		    "CLIENT_SECRET '...');");
	}

	ApplyDefaultScope(*result);

	// access_token/refresh_token are absent at creation and written back after
	// the flow completes; they are redacted from the outset so there is no
	// window in which a refreshed token could be displayed.
	result->redact_keys.insert("client_secret");
	result->redact_keys.insert("access_token");
	result->redact_keys.insert("refresh_token");

	return std::move(result);
}

void RegisterCommonParameters(CreateSecretFunction &function) {
	function.named_parameters["drive_id"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["root_folder_id"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["drive_scope"] = LogicalType(LogicalTypeId::VARCHAR);
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
	// B-4 / D-10: credential_chain is the default, so `CREATE SECRET (TYPE
	// gdrive);` works with no arguments for anyone who has run `gcloud auth
	// application-default login`. This changed from "config", which could
	// never succeed argument-less (config demands a token), so no working
	// statement changes meaning.
	type.default_provider = "credential_chain";
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

	CreateSecretFunction chain_fn = {kSecretTypeName, "credential_chain", CreateCredentialChainSecret, {}};
	RegisterCommonParameters(chain_fn);
	loader.RegisterFunction(chain_fn);

	CreateSecretFunction authcode_fn = {kSecretTypeName, "authorization_code", CreateAuthorizationCodeSecret, {}};
	authcode_fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
	authcode_fn.named_parameters["client_secret"] = LogicalType(LogicalTypeId::VARCHAR);
	authcode_fn.named_parameters["redirect_port"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(authcode_fn);
	loader.RegisterFunction(authcode_fn);
}

} // namespace gdrive
} // namespace duckdb
