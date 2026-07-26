// S-1.4 / S-1.7 / S-1.8 -- the Wave 1 <-> Wave 2 seam.
//
// Implements GDriveAuthContext GetAuthContext(...) and HasAnyGDriveSecret(...)
// from the frozen src/include/gdrive_auth.hpp. Resolves a gdrive:// path to a
// registered `gdrive` secret (via DuckDB's scope-matching secret manager),
// then produces a usable bearer token for whichever provider that secret
// uses:
//
//   PROVIDER config           -- returns ACCESS_TOKEN as stored. No minting,
//                                 no refresh (S-1.1/S-1.2 scope); documented
//                                 as a known gap in the report for this slice.
//   PROVIDER service_account  -- reads KEY_FILE, mints a token via RFC 7523
//                                 (gdrive_service_account_duckdb.cpp), and
//                                 caches it in memory keyed by secret name so
//                                 repeated FileSystem calls do not re-mint on
//                                 every request (REQ-NF-02).
//
// REQ-NF-03: no path in this file ever puts a token, a client secret, or key
// material into an exception message.
#include "gdrive_auth.hpp"
#include "gdrive_service_account.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Forward declaration of gdrive_service_account_duckdb.cpp's minting
// function. There is no shared header for it: MintServiceAccountToken is not
// part of the frozen gdrive_service_account.hpp contract (that header is
// pure-only, per its own doc comment), so the two DuckDB-coupled TUs agree on
// this signature directly. Both are this track's files, so keeping them in
// sync is a local concern, not a cross-track one.
// ---------------------------------------------------------------------------
struct MintedToken {
	bool ok = false;
	std::string error;
	std::string access_token;
	int64_t expires_at_unix = 0;
};

MintedToken MintServiceAccountToken(const ServiceAccountKey &key, const std::string &scope, const std::string &subject,
                                     int64_t now_unix);

namespace {

// ---------------------------------------------------------------------------
// In-memory token cache, keyed by secret name. Re-minting an RS256 assertion
// and POSTing it to Google on every single Read()/OpenFile() call would be a
// quota disaster (REQ-NF-02) -- a token is valid ~1h, so it is cached until
// shortly before it expires and only re-minted then.
//
// Policy: re-mint when fewer than kRefreshSlackSeconds remain, not exactly at
// expiry -- a request that starts just before expiry must not race a token
// that dies mid-flight.
// ---------------------------------------------------------------------------
constexpr int64_t kRefreshSlackSeconds = 60;

struct CachedToken {
	std::string access_token;
	int64_t expires_at_unix = 0;
};

std::mutex &CacheMutex() {
	static std::mutex m;
	return m;
}

std::unordered_map<std::string, CachedToken> &TokenCache() {
	static std::unordered_map<std::string, CachedToken> cache;
	return cache;
}

int64_t NowUnix() {
	return static_cast<int64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string GetOrEmpty(const KeyValueSecret &kv, const char *key) {
	Value v;
	if (kv.TryGetValue(key, v) && !v.IsNull()) {
		return v.ToString();
	}
	return "";
}

// REQ-NF-03 / S-1.8: distinct, actionable message for a missing/unreadable
// key file. Never reads the file's *contents* into this message -- only the
// path, which is not secret material.
std::string ReadFileOrThrow(const std::string &path, const std::string &secret_name) {
	std::ifstream f(path, std::ios::binary);
	if (!f.good()) {
		throw IOException(
		    "gdrive secret '%s': service-account key file not found or not readable: '%s'. "
		    "Check the KEY_FILE path, or recreate the secret, e.g.:\n"
		    "  CREATE SECRET %s (TYPE gdrive, PROVIDER service_account, KEY_FILE '/etc/creds/sa.json');",
		    secret_name.c_str(), path.c_str(), secret_name.c_str());
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

GDriveAuthContext BuildContextFromServiceAccount(const KeyValueSecret &kv, const std::string &secret_name) {
	const std::string key_file = GetOrEmpty(kv, "key_file");
	if (key_file.empty()) {
		throw InvalidInputException("gdrive secret '%s' (PROVIDER service_account) is missing KEY_FILE.",
		                             secret_name.c_str());
	}
	std::string scope = GetOrEmpty(kv, "scope");
	if (scope.empty()) {
		scope = SCOPE_DRIVE_READONLY; // REQ-NF-04
	}

	// Fast path: a cached, not-about-to-expire token needs no file I/O, no
	// signing, and no network call at all (REQ-NF-02).
	{
		std::lock_guard<std::mutex> lock(CacheMutex());
		auto it = TokenCache().find(secret_name);
		if (it != TokenCache().end() && it->second.expires_at_unix - kRefreshSlackSeconds > NowUnix()) {
			GDriveAuthContext ctx;
			ctx.access_token = it->second.access_token;
			ctx.drive_id = GetOrEmpty(kv, "drive_id");
			ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
			ctx.scope = scope;
			ctx.secret_name = secret_name;
			return ctx;
		}
	}

	std::string key_json = ReadFileOrThrow(key_file, secret_name);
	ServiceAccountKeyParse parsed = ParseServiceAccountKey(key_json);
	key_json.clear(); // never keep the raw key text around longer than needed

	if (!parsed.ok) {
		// ParseServiceAccountKey guarantees parsed.error never quotes key
		// material (see its own doc comment and S-1.8's Catch2 coverage).
		throw InvalidInputException("gdrive secret '%s': invalid service-account key file '%s': %s",
		                             secret_name.c_str(), key_file.c_str(), parsed.error.c_str());
	}

	const int64_t now = NowUnix();
	MintedToken minted = MintServiceAccountToken(parsed.key, scope, /*subject=*/"", now);
	if (!minted.ok) {
		throw IOException("gdrive secret '%s': failed to obtain an access token from Google: %s", secret_name.c_str(),
		                   minted.error.c_str());
	}

	{
		std::lock_guard<std::mutex> lock(CacheMutex());
		TokenCache()[secret_name] = CachedToken {minted.access_token, minted.expires_at_unix};
	}

	GDriveAuthContext ctx;
	ctx.access_token = minted.access_token;
	ctx.drive_id = GetOrEmpty(kv, "drive_id");
	ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
	ctx.scope = scope;
	ctx.secret_name = secret_name;
	return ctx;
}

GDriveAuthContext BuildContextFromConfig(const KeyValueSecret &kv, const std::string &secret_name) {
	// S-1.1/S-1.2 scope: PROVIDER config is a pass-through for pre-obtained
	// tokens. It does not refresh via REFRESH_TOKEN/CLIENT_ID/CLIENT_SECRET
	// yet -- that needs a generic OAuth2 refresh POST, which is exactly the
	// `authorization_code` provider's job once datazoo-oauth2 is wired in
	// (see this slice's report). Documented gap, not a silent omission.
	const std::string access_token = GetOrEmpty(kv, "access_token");
	if (access_token.empty()) {
		throw InvalidInputException(
		    "gdrive secret '%s' (PROVIDER config) has no usable ACCESS_TOKEN (and no refresh path is implemented "
		    "yet for a bare REFRESH_TOKEN). Recreate it with a fresh token, e.g.:\n"
		    "  CREATE SECRET %s (TYPE gdrive, PROVIDER config, ACCESS_TOKEN '...');",
		    secret_name.c_str(), secret_name.c_str());
	}

	std::string scope = GetOrEmpty(kv, "scope");
	if (scope.empty()) {
		scope = SCOPE_DRIVE_READONLY;
	}

	GDriveAuthContext ctx;
	ctx.access_token = access_token;
	ctx.drive_id = GetOrEmpty(kv, "drive_id");
	ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
	ctx.scope = scope;
	ctx.secret_name = secret_name;
	return ctx;
}

} // namespace

GDriveAuthContext GetAuthContext(ClientContext &context, const std::string &path) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, path, "gdrive");

	if (!match.HasMatch()) {
		// REQ from this slice: name the path and show a CREATE SECRET example
		// rather than letting an opaque 401 come back from Google.
		throw IOException(
		    "gdrive: no secret configured for path '%s'. Create one first, e.g.:\n"
		    "  CREATE SECRET gdrive_sa (TYPE gdrive, PROVIDER service_account, "
		    "KEY_FILE '/etc/creds/sa.json');\n"
		    "or, with a pre-obtained token:\n"
		    "  CREATE SECRET gdrive_tok (TYPE gdrive, PROVIDER config, ACCESS_TOKEN '...');",
		    path.c_str());
	}

	const BaseSecret &base = match.GetSecret();
	const auto *kv = dynamic_cast<const KeyValueSecret *>(&base);
	if (!kv) {
		throw InvalidInputException("gdrive secret '%s' is not a key-value secret", base.GetName().c_str());
	}

	const std::string secret_name = base.GetName().GetIdentifierName();

	if (base.GetProvider() == "service_account") {
		return BuildContextFromServiceAccount(*kv, secret_name);
	}
	if (base.GetProvider() == "config") {
		return BuildContextFromConfig(*kv, secret_name);
	}

	throw InvalidInputException("gdrive secret '%s' has unsupported provider '%s'; expected 'service_account' or "
	                             "'config' ('authorization_code' is not yet implemented -- see docs/hld.md section 5).",
	                             secret_name.c_str(), base.GetProvider().c_str());
}

bool HasAnyGDriveSecret(ClientContext &context) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secrets = secret_manager.AllSecrets(transaction);
	for (const auto &entry : secrets) {
		if (entry.secret && entry.secret->GetType() == "gdrive") {
			return true;
		}
	}
	return false;
}

} // namespace gdrive
} // namespace duckdb
