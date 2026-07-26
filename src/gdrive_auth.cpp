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

// Windows headers define min/max macros that conflict with C++ std:: functions.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

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

// ---------------------------------------------------------------------------
// PROVIDER config's refresh path: `grant_type=refresh_token` against Google's
// token endpoint. Shares the *cache* (TokenCache() above) with the
// service_account path -- same kRefreshSlackSeconds policy, same map keyed by
// secret name -- deliberately: two token-acquisition strategies, one cache.
//
// The HTTP/parsing helpers below intentionally mirror
// gdrive_service_account_duckdb.cpp's (SplitUrl / UrlEncodeFormValue /
// TokenResponse parsing): that file's copies live in its own anonymous
// namespace with no shared header (its MintedToken struct is forward-declared
// ad hoc, not a frozen contract -- see this file's header comment), so
// duplicating these small, stable helpers here is cheaper than inventing a
// shared header for them mid-wave.
// ---------------------------------------------------------------------------

bool SplitUrl(const std::string &url, std::string &scheme_host_port, std::string &path) {
	auto scheme_end = url.find("://");
	if (scheme_end == std::string::npos) {
		return false;
	}
	auto path_start = url.find('/', scheme_end + 3);
	if (path_start == std::string::npos) {
		scheme_host_port = url;
		path = "/";
	} else {
		scheme_host_port = url.substr(0, path_start);
		path = url.substr(path_start);
	}
	return !scheme_host_port.empty();
}

std::string UrlEncodeFormValue(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	static const char *hex = "0123456789ABCDEF";
	for (unsigned char c : value) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.' || c == '~') {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(hex[(c >> 4) & 0xF]);
			out.push_back(hex[c & 0xF]);
		}
	}
	return out;
}

struct RefreshTokenResponse {
	bool parsed_ok = false;
	std::string access_token;
	int64_t expires_in = 0;
	std::string error;             // OAuth "error" field, e.g. "invalid_grant"
	std::string error_description; // OAuth "error_description" field
};

RefreshTokenResponse ParseRefreshTokenResponse(const std::string &body) {
	RefreshTokenResponse result;
	picojson::value root;
	std::string parse_err = picojson::parse(root, body);
	if (!parse_err.empty() || !root.is<picojson::object>()) {
		return result;
	}
	const auto &obj = root.get<picojson::object>();

	auto find_string = [&](const char *key) -> std::string {
		auto it = obj.find(key);
		if (it != obj.end() && it->second.is<std::string>()) {
			return it->second.get<std::string>();
		}
		return "";
	};

	result.access_token = find_string("access_token");
	result.error = find_string("error");
	result.error_description = find_string("error_description");

	auto expires_it = obj.find("expires_in");
	if (expires_it != obj.end() && expires_it->second.is<double>()) {
		result.expires_in = static_cast<int64_t>(expires_it->second.get<double>());
	}

	result.parsed_ok = true;
	return result;
}

//! Result of a refresh-token POST. Never carries the request body, the
//! refresh token, or the client secret in `error` (REQ-NF-03) -- only
//! Google's own small `error`/`error_description` fields, which name a
//! failure mode ("invalid_grant") and never echo request content.
struct RefreshResult {
	bool ok = false;
	std::string error;
	std::string access_token;
	int64_t expires_at_unix = 0;
};

RefreshResult RefreshUserToken(const std::string &client_id, const std::string &client_secret,
                                const std::string &refresh_token, int64_t now_unix) {
	RefreshResult result;

	std::string scheme_host_port, path;
	if (!SplitUrl(GOOGLE_TOKEN_URL, scheme_host_port, path)) {
		result.error = "internal error: GOOGLE_TOKEN_URL is not a valid URL";
		return result;
	}

	std::string body = "grant_type=refresh_token" + ("&client_id=" + UrlEncodeFormValue(client_id)) +
	                    ("&client_secret=" + UrlEncodeFormValue(client_secret)) +
	                    ("&refresh_token=" + UrlEncodeFormValue(refresh_token));

	duckdb_httplib_openssl::Client client(scheme_host_port);
	client.set_connection_timeout(30);
	client.set_read_timeout(30);
	client.set_write_timeout(30);
	client.set_follow_location(true);

	auto response = client.Post(path.c_str(), body, "application/x-www-form-urlencoded");
	if (!response) {
		result.error =
		    "no response from the Google token endpoint (" + std::string(GOOGLE_TOKEN_URL) + "); check network connectivity";
		return result;
	}

	// REQ-NF-03: the response body is Google-authored and small, but it is
	// never interpolated *raw* -- only the two well-known named fields
	// (error / error_description) are lifted out, so nothing the request
	// echoed back (there is nothing to echo for this grant type: no token
	// material appears in a token-endpoint error body) can leak verbatim.
	auto parsed = ParseRefreshTokenResponse(response->body);
	if (response->status != 200) {
		std::string detail = parsed.parsed_ok && !parsed.error_description.empty() ? parsed.error_description
		                      : parsed.parsed_ok && !parsed.error.empty()          ? parsed.error
		                                                                           : "";
		result.error = "Google token endpoint returned HTTP " + std::to_string(response->status) +
		                (detail.empty() ? std::string() : (": " + detail));
		return result;
	}

	if (!parsed.parsed_ok || parsed.access_token.empty()) {
		result.error = "Google token endpoint response did not contain an access_token";
		return result;
	}

	result.ok = true;
	result.access_token = parsed.access_token;
	int64_t lifetime = parsed.expires_in > 0 ? parsed.expires_in : 3600;
	result.expires_at_unix = now_unix + lifetime;
	return result;
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
	std::string scope = GetOrEmpty(kv, "drive_scope");
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
	std::string scope = GetOrEmpty(kv, "drive_scope");
	if (scope.empty()) {
		scope = SCOPE_DRIVE_READONLY;
	}

	auto make_ctx = [&](const std::string &access_token) {
		GDriveAuthContext ctx;
		ctx.access_token = access_token;
		ctx.drive_id = GetOrEmpty(kv, "drive_id");
		ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
		ctx.scope = scope;
		ctx.secret_name = secret_name;
		return ctx;
	};

	// An explicit ACCESS_TOKEN is always the pass-through case: "I already
	// have a token, use it as-is." No minting, no cache lookup -- the caller
	// is responsible for it being fresh. This preserves the original
	// PROVIDER config behaviour exactly.
	const std::string access_token = GetOrEmpty(kv, "access_token");
	if (!access_token.empty()) {
		return make_ctx(access_token);
	}

	// No ACCESS_TOKEN: fall back to REFRESH_TOKEN + CLIENT_ID + CLIENT_SECRET,
	// minting a fresh one via `grant_type=refresh_token` against Google's
	// token endpoint. This is the delegated-user path a service account
	// needs to write outside a Shared Drive (it has no storage quota of its
	// own -- see docs/implementation-plan.md's live-test-credentials note).
	const std::string refresh_token = GetOrEmpty(kv, "refresh_token");
	const std::string client_id = GetOrEmpty(kv, "client_id");
	const std::string client_secret = GetOrEmpty(kv, "client_secret");
	if (refresh_token.empty() || client_id.empty() || client_secret.empty()) {
		throw InvalidInputException(
		    "gdrive secret '%s' (PROVIDER config) has no usable ACCESS_TOKEN, and refreshing needs all three of "
		    "REFRESH_TOKEN, CLIENT_ID and CLIENT_SECRET (at least one is missing). Recreate it with either a fresh "
		    "token:\n"
		    "  CREATE SECRET %s (TYPE gdrive, PROVIDER config, ACCESS_TOKEN '...');\n"
		    "or a refreshable one:\n"
		    "  CREATE SECRET %s (TYPE gdrive, PROVIDER config, CLIENT_ID '...', CLIENT_SECRET '...', "
		    "REFRESH_TOKEN '...');",
		    secret_name.c_str(), secret_name.c_str(), secret_name.c_str());
	}

	// Fast path: a cached, not-about-to-expire token needs no network call
	// at all (REQ-NF-02). Same cache, same slack policy as the
	// service_account path above -- one mechanism, two mint strategies.
	{
		std::lock_guard<std::mutex> lock(CacheMutex());
		auto it = TokenCache().find(secret_name);
		if (it != TokenCache().end() && it->second.expires_at_unix - kRefreshSlackSeconds > NowUnix()) {
			return make_ctx(it->second.access_token);
		}
	}

	RefreshResult refreshed = RefreshUserToken(client_id, client_secret, refresh_token, NowUnix());
	if (!refreshed.ok) {
		// REQ-NF-03: RefreshResult::error never contains the refresh token,
		// the client secret, or a raw echoed request/response body -- see
		// RefreshUserToken's own doc comment.
		throw IOException("gdrive secret '%s': failed to refresh an access token from Google: %s",
		                   secret_name.c_str(), refreshed.error.c_str());
	}

	{
		std::lock_guard<std::mutex> lock(CacheMutex());
		TokenCache()[secret_name] = CachedToken {refreshed.access_token, refreshed.expires_at_unix};
	}

	return make_ctx(refreshed.access_token);
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

	// BaseSecret::GetName() returns `const string &` on v1.5.3 and v1.4.4 --
	// the versions this extension actually ships against. It became
	// `const Identifier &` (with .GetIdentifierName()) only on DuckDB main,
	// well after both. Calling the newer API compiled fine locally, because
	// the duckdb submodule had drifted ~9600 commits past its pinned tag, and
	// broke every release build. Keep this on the released API.
	const std::string secret_name = base.GetName();

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
