// S-1.4 / S-1.7 / S-1.8 -- the Wave 1 <-> Wave 2 seam.
//
// Implements GDriveAuthContext GetAuthContext(...) and HasAnyGDriveSecret(...)
// from the frozen src/include/gdrive_auth.hpp. Resolves a gdrive:// path to a
// registered `gdrive` secret (via DuckDB's scope-matching secret manager),
// then produces a usable bearer token for whichever provider that secret
// uses:
//
//   PROVIDER config           -- uses ACCESS_TOKEN as stored. With
//                                 REFRESH_TOKEN/CLIENT_ID/CLIENT_SECRET beside
//                                 it, a 401 is answered by the refresh grant;
//                                 without them the 401 stands. With no
//                                 ACCESS_TOKEN, mints from the refresh token.
//   PROVIDER service_account  -- reads KEY_FILE, mints a token via RFC 7523
//                                 (gdrive_service_account_duckdb.cpp), and
//                                 caches it in memory keyed by a fingerprint
//                                 of the credential material so repeated
//                                 FileSystem calls do not re-mint on every
//                                 request (REQ-NF-02). See FingerprintKey --
//                                 the key is NOT the secret name.
//
// Every provider produces a GDriveTokenSource, not a token: handles keep the
// source, so one that outlives its token (a long scan, an ATTACH) re-mints
// rather than failing with 401 an hour in. See CachedTokenSource below.
//
// REQ-NF-03: no path in this file ever puts a token, a client secret, or key
// material into an exception message.
// ---------------------------------------------------------------------------
// THESE DEFINES MUST COME BEFORE EVERY #include IN THIS FILE.
//
// NOMINMAX suppresses windows.h's min/max macros, which otherwise break every
// later std::min / std::max. It only works if it is defined before ANYTHING
// pulls windows.h in -- and several headers below do, transitively:
// datazoo/oauth2/oauth2_flow_v2.hpp -> http_client.hpp -> httplib.hpp ->
// windows.h. Putting these defines after those includes leaves them with no
// effect at all, silently, on Windows only.
//
// That is not hypothetical. Adding the datazoo-oauth2 includes ABOVE this
// block broke the Windows v1.4.5 LTS build while every other job in the
// matrix passed, and while the same library built cleanly on Windows inside
// erpl-web -- whose sources do not define NOMINMAX after including it.
//
// Same class of ordering trap as PICOJSON_USE_INT64 (see CMakeLists.txt): a
// macro that must be set before a header's first inclusion, whose failure
// mode is silence rather than an error.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include "gdrive_adc.hpp"
#include "gdrive_auth.hpp"
#include "gdrive_credential_file.hpp"
#include "gdrive_identity.hpp"
#include "duckdb/common/string_util.hpp"
#include "gdrive_oauth_params.hpp"
#include "gdrive_service_account.hpp"
#include "gdrive_stats.hpp"
#include "gdrive_token_policy.hpp"

// datazoo-oauth2 -- the shared, provider-agnostic OAuth2 stack (D-11).
#include "datazoo/oauth2/oauth2_flow_v2.hpp"
#include "datazoo/oauth2/oauth2_secret_token_manager.hpp"
#include "datazoo/oauth2/oauth2_types.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include "httplib.hpp"

#include <cstdint>
#include <jwt-cpp/traits/kazuho-picojson/defaults.h>
#include <openssl/evp.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

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
// In-memory token cache. Re-minting an RS256 assertion
// and POSTing it to Google on every single Read()/OpenFile() call would be a
// quota disaster (REQ-NF-02) -- a token is valid ~1h, so it is cached until
// shortly before it expires and only re-minted then.
//
// Policy: re-mint when fewer than kTokenRefreshSlackSeconds remain, not
// exactly at expiry (TokenIsFresh, gdrive_token_policy.hpp). Only the token
// sources below touch this map.
// ---------------------------------------------------------------------------
std::mutex &CacheMutex() {
	static std::mutex m;
	return m;
}

std::unordered_map<std::string, CachedTokenState> &TokenCache() {
	static std::unordered_map<std::string, CachedTokenState> cache;
	return cache;
}

//! How long a failed mint is remembered (and rethrown) before the token
//! endpoint is tried again for that credential. See MintAndStore.
constexpr int64_t kMintFailureBackoffSeconds = 30;

struct MintFailure {
	std::exception_ptr error;
	int64_t until_unix = 0;
};

//! Guarded by CacheMutex(), like TokenCache().
std::unordered_map<std::string, MintFailure> &MintFailures() {
	static std::unordered_map<std::string, MintFailure> failures;
	return failures;
}

//! One mint lock per cache key, shared by every source for that credential:
//! N open handles on one credential must mint ONE token on expiry or after a
//! 401, not N. Held across the token-endpoint call; the cache mutex is not.
std::shared_ptr<std::mutex> MintLockFor(const std::string &cache_key) {
	static std::mutex locks_guard;
	static std::unordered_map<std::string, std::shared_ptr<std::mutex>> locks;
	std::lock_guard<std::mutex> guard(locks_guard);
	auto &lock = locks[cache_key];
	if (!lock) {
		lock = std::make_shared<std::mutex>();
	}
	return lock;
}

// ---------------------------------------------------------------------------
// Cache key. NOT the secret name.
//
// This cache is `static` -- one map for the whole PROCESS, shared by every
// DuckDB instance in it. Keying it by secret name alone meant:
//
//   * database A creates SECRET gdrive for service account A, caches token A;
//   * database B, same process, creates SECRET gdrive for service account B;
//   * B's next request finds "gdrive" in the cache and uses A's token, while
//     applying B's drive_id.
//
// That is one tenant reading another tenant's Drive with the other tenant's
// credential. The path-resolution cache was designed against exactly this
// (see CacheKey in gdrive_filesystem.hpp) and this cache was missed.
//
// So the key is the name PLUS a fingerprint of everything that determines
// which token comes back. The fingerprint is hashed, so no credential
// material sits in a map key (REQ-NF-03), and it is a SHA-256 so distinct
// inputs cannot collide into a shared token.
std::string FingerprintKey(const std::string &secret_name, const std::string &provider,
                            const std::vector<std::string> &material) {
	std::string joined;
	for (const auto &m : material) {
		joined += m;
		joined.push_back('\x1f'); // unit separator: cannot appear in these values
	}
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	std::string hex;
	if (EVP_Digest(joined.data(), joined.size(), digest, &digest_len, EVP_sha256(), nullptr) == 1) {
		static const char *kHex = "0123456789abcdef";
		hex.reserve(digest_len * 2);
		for (unsigned int i = 0; i < digest_len; i++) {
			hex.push_back(kHex[(digest[i] >> 4) & 0xF]);
			hex.push_back(kHex[digest[i] & 0xF]);
		}
	} else {
		// Hashing must never fail open onto a name-only key -- that is the
		// cross-tenant bug. A key nothing else can equal simply misses the
		// cache, costing a re-mint.
		hex = "unhashable-" + std::to_string(reinterpret_cast<uintptr_t>(joined.data()));
	}
	return provider + "\x1f" + secret_name + "\x1f" + hex;
}

//! Identity of a key file for cache purposes: path plus size plus mtime.
//!
//! Path alone would keep serving a stale token after a key is ROTATED IN
//! PLACE, which is the normal way keys get rotated. Hashing the file's
//! contents would be exact but reintroduces a read on every call, which is
//! what the cache exists to avoid; a stat is cheap.
std::string KeyFileStamp(const std::string &key_file) {
	struct stat st;
	if (stat(key_file.c_str(), &st) != 0) {
		return key_file; // let the later open() produce the real error
	}
	// Path + size + mtime alone can collide (a credential replaced within the
	// same second at the same size); the inode and ctime change on every
	// replacement. This stamp keys the path and block caches, not just tokens.
	return key_file + ":" + std::to_string(static_cast<long long>(st.st_size)) + ":" +
	       std::to_string(static_cast<long long>(st.st_mtime)) + ":" +
	       std::to_string(static_cast<long long>(st.st_ctime)) + ":" +
	       std::to_string(static_cast<unsigned long long>(st.st_ino));
}

int64_t NowUnix() {
	return static_cast<int64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// Token sources (GDriveTokenSource, gdrive_auth.hpp).
//
// A mint function obtains a token from whichever endpoint the provider uses
// and THROWS a DuckDB exception naming the secret when it cannot. A source
// without one has nothing to refresh with.
// ---------------------------------------------------------------------------
using MintFn = std::function<CachedTokenState()>;

//! Every minting provider: cached per credential fingerprint in TokenCache(),
//! re-minted a minute before expiry, or on demand after a 401.
class CachedTokenSource : public GDriveTokenSource {
public:
	//! `seed`, when non-empty, is a pasted ACCESS_TOKEN: used first, with an
	//! unknown lifetime, and replaced by a minted one once Drive rejects it.
	//! Seeded only if the cache holds nothing for this key -- a seed rejected
	//! earlier in the process has already been replaced.
	CachedTokenSource(std::string cache_key_p, MintFn mint_p, int64_t max_age_seconds_p, const std::string &seed = "")
	    : cache_key(std::move(cache_key_p)), mint(std::move(mint_p)), max_age_seconds(max_age_seconds_p),
	      mint_lock(MintLockFor(cache_key)) {
		if (!seed.empty()) {
			std::lock_guard<std::mutex> guard(CacheMutex());
			auto &cache = TokenCache();
			if (cache.find(cache_key) == cache.end()) {
				cache[cache_key] = CachedTokenState {seed, kUnknownExpiry, NowUnix()};
			}
		}
	}

	std::string Token() override {
		std::string cached;
		if (TryFresh(cached)) {
			return cached;
		}
		if (!mint) {
			// Nothing to mint with: hand out what there is, stale or not, and
			// let Drive's 401 say so.
			std::lock_guard<std::mutex> guard(CacheMutex());
			auto it = TokenCache().find(cache_key);
			return it == TokenCache().end() ? std::string() : it->second.access_token;
		}
		// One mint at a time per credential; whoever waited finds the token
		// the first one minted.
		std::lock_guard<std::mutex> mint_guard(*mint_lock);
		if (TryFresh(cached)) {
			return cached;
		}
		return MintAndStore();
	}

	std::string Refresh(const std::string &rejected) override {
		std::lock_guard<std::mutex> mint_guard(*mint_lock);
		{
			std::lock_guard<std::mutex> guard(CacheMutex());
			auto &cache = TokenCache();
			auto it = cache.find(cache_key);
			const bool has_cached = it != cache.end();
			switch (DecideAfterRejection(has_cached, has_cached ? it->second : CachedTokenState(), rejected,
			                             NowUnix(), max_age_seconds, static_cast<bool>(mint))) {
			case RejectionAction::REUSE_CACHED:
				return it->second.access_token;
			case RejectionAction::GIVE_UP:
				return std::string();
			case RejectionAction::MINT:
				// The rejected entry is NOT erased: a successful mint overwrites
				// it, and a failed one must not turn into "nothing cached".
				break;
			}
		}
		return MintAndStore();
	}

private:
	bool TryFresh(std::string &out) {
		std::lock_guard<std::mutex> guard(CacheMutex());
		auto it = TokenCache().find(cache_key);
		if (it != TokenCache().end() && TokenIsFresh(it->second, NowUnix(), max_age_seconds)) {
			out = it->second.access_token;
			return true;
		}
		return false;
	}

	//! Caller holds mint_lock.
	//!
	//! A failed mint is remembered for kMintFailureBackoffSeconds and rethrown
	//! to every caller in that window. Without it a revoked credential costs
	//! one doomed token-endpoint POST per Drive request, on every handle and
	//! thread -- the 401 path makes a mint attempt on each one.
	std::string MintAndStore() {
		{
			std::lock_guard<std::mutex> guard(CacheMutex());
			auto it = MintFailures().find(cache_key);
			if (it != MintFailures().end()) {
				if (it->second.until_unix > NowUnix()) {
					std::rethrow_exception(it->second.error);
				}
				MintFailures().erase(it);
			}
		}
		IncrementGlobalTokenRequest();
		CachedTokenState minted;
		try {
			minted = mint();
		} catch (...) {
			std::lock_guard<std::mutex> guard(CacheMutex());
			MintFailures()[cache_key] = MintFailure {std::current_exception(), NowUnix() + kMintFailureBackoffSeconds};
			throw;
		}
		minted.minted_at_unix = NowUnix();
		std::lock_guard<std::mutex> guard(CacheMutex());
		TokenCache()[cache_key] = minted;
		return minted.access_token;
	}

	std::string cache_key;
	MintFn mint;
	//! gdrive_debug_token_max_age_seconds, captured at resolution (a handle has
	//! no opener later). 0 = Google's own lifetime.
	int64_t max_age_seconds;
	std::shared_ptr<std::mutex> mint_lock;
};

//! PROVIDER config with a bare ACCESS_TOKEN: the caller owns its lifetime, and
//! there is nothing to refresh it with. Kept out of the process-wide cache.
class StaticTokenSource : public GDriveTokenSource {
public:
	explicit StaticTokenSource(std::string token_p) : token(std::move(token_p)) {
	}
	std::string Token() override {
		return token;
	}
	std::string Refresh(const std::string &) override {
		return std::string(); // the 401 stands
	}

private:
	std::string token;
};

// ---------------------------------------------------------------------------
// PROVIDER config's refresh path: `grant_type=refresh_token` against Google's
// token endpoint. Shares the *cache* (TokenCache() above) with the
// service_account path -- same freshness policy, same map, same
// FingerprintKey scheme, same CachedTokenSource -- deliberately: several
// token strategies, one cache.
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
		// Keep BOTH fields when both are present. `error` is the machine
		// code ("invalid_grant") and `error_description` is the prose, and
		// Google's prose alone is often useless -- a malformed refresh token
		// yields the literal description "Bad Request", which tells the
		// reader nothing that the HTTP status did not.
		std::string detail;
		if (parsed.parsed_ok) {
			if (!parsed.error.empty() && !parsed.error_description.empty()) {
				detail = parsed.error + ": " + parsed.error_description;
			} else if (!parsed.error_description.empty()) {
				detail = parsed.error_description;
			} else {
				detail = parsed.error;
			}
		}
		result.error = "Google token endpoint returned HTTP " + std::to_string(response->status) +
		                (detail.empty() ? std::string() : (": " + detail));
		// invalid_grant is the one every long-lived deployment eventually
		// meets: Google revokes a refresh token when the user withdraws
		// access, when the OAuth client is deleted, or after ~6 months unused
		// on a project still in "Testing" publishing status. CLAUDE.md calls
		// that expected maintenance rather than a bug -- so the message should
		// say what to do about it instead of leaving the reader with a bare
		// 400.
		if (parsed.parsed_ok && parsed.error == "invalid_grant") {
			result.error +=
			    "\n\nThe refresh token is no longer valid. Google revokes one when access is "
			    "withdrawn, when the OAuth client is deleted, or after ~6 months unused while the "
			    "project is still in \"Testing\" publishing status. Obtain a new one (for this repo, "
			    "`make oauth_consent`) and recreate the secret. A service-account key does not "
			    "expire this way.";
		}
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

//! Exchange a workload-identity subject token for a Drive access token.
//!
//! Two hops, and BOTH are required for Drive. STS federates the platform's
//! subject token into a short-lived federated token, which by itself carries
//! only `cloud-platform`; the impersonation call is what mints a token for a
//! real service account with the Drive scope. Skipping the second hop yields
//! a token Drive rejects with a 403 that reads like a permissions problem.
//!
//! The subject token is read HERE, not at parse time. A projected Kubernetes
//! token is rotated under the process (hourly), so a value captured when the
//! secret was created works until it silently does not.
RefreshResult ExchangeExternalAccountToken(const AdcExternalAccount &ext, const std::string &scope,
                                            int64_t now_unix) {
	RefreshResult result;

	// The platform writes this; a missing file means the pod is not configured
	// for federation, which is worth saying plainly rather than as an STS 400.
	std::string subject_token, why;
	if (!ReadCredentialFile(ext.subject_token_path, subject_token, why)) {
		result.error = "no workload-identity token at '" + ext.subject_token_path + "' (" + why +
		                "). This path is written by the platform (a projected service-account "
		                "token, on Kubernetes); if it is absent the workload is not federated.";
		return result;
	}
	// A projected token file ends with a newline often enough to matter, and
	// STS rejects the token rather than trimming it.
	while (!subject_token.empty() && (subject_token.back() == '\n' || subject_token.back() == '\r')) {
		subject_token.pop_back();
	}
	if (subject_token.empty()) {
		result.error = "the workload-identity token at '" + ext.subject_token_path + "' is empty";
		return result;
	}

	// --- hop 1: STS token exchange -----------------------------------------
	std::string sts_host, sts_path;
	if (!SplitUrl(ext.token_url, sts_host, sts_path)) {
		result.error = "the credentials name an unusable token_url";
		return result;
	}
	// cloud-platform, NOT the Drive scope: a federated token may only carry
	// scopes the pool grants, and the Drive scope is applied at impersonation.
	const std::string sts_body =
	    "grant_type=" + UrlEncodeFormValue("urn:ietf:params:oauth:grant-type:token-exchange") +
	    "&audience=" + UrlEncodeFormValue(ext.audience) +
	    "&scope=" + UrlEncodeFormValue("https://www.googleapis.com/auth/cloud-platform") +
	    "&requested_token_type=" + UrlEncodeFormValue("urn:ietf:params:oauth:token-type:access_token") +
	    "&subject_token_type=" + UrlEncodeFormValue(ext.subject_token_type) +
	    "&subject_token=" + UrlEncodeFormValue(subject_token);

	duckdb_httplib_openssl::Client sts(sts_host);
	sts.set_connection_timeout(30);
	sts.set_read_timeout(30);
	sts.set_write_timeout(30);
	sts.set_follow_location(true);
	auto sts_response = sts.Post(sts_path.c_str(), sts_body, "application/x-www-form-urlencoded");
	if (!sts_response) {
		result.error = "no response from the STS endpoint (" + ext.token_url + "); check network connectivity";
		return result;
	}
	auto sts_parsed = ParseRefreshTokenResponse(sts_response->body);
	if (sts_response->status != 200) {
		std::string detail;
		if (sts_parsed.parsed_ok) {
			detail = sts_parsed.error_description.empty() ? sts_parsed.error : sts_parsed.error_description;
		}
		result.error = "STS token exchange returned HTTP " + std::to_string(sts_response->status) +
		                (detail.empty() ? std::string() : (": " + detail)) +
		                "\n\nA rejected subject token usually means the audience does not match the "
		                "workload identity pool provider this token was minted for.";
		return result;
	}
	if (!sts_parsed.parsed_ok || sts_parsed.access_token.empty()) {
		result.error = "the STS response contained no access_token";
		return result;
	}

	// --- hop 2: impersonate the service account, WITH the Drive scope ------
	if (ext.service_account_impersonation_url.empty()) {
		result.error =
		    "these credentials federate an identity but name no "
		    "service_account_impersonation_url, so no token can carry the Drive scope. Add a "
		    "service account to the credential configuration.";
		return result;
	}
	std::string imp_host, imp_path;
	if (!SplitUrl(ext.service_account_impersonation_url, imp_host, imp_path)) {
		result.error = "the credentials name an unusable service_account_impersonation_url";
		return result;
	}
	picojson::object imp_body_obj;
	picojson::array scopes;
	scopes.emplace_back(scope);
	imp_body_obj["scope"] = picojson::value(scopes);
	imp_body_obj["lifetime"] = picojson::value(std::string("3600s"));
	const std::string imp_body = picojson::value(imp_body_obj).serialize();

	duckdb_httplib_openssl::Client imp(imp_host);
	imp.set_connection_timeout(30);
	imp.set_read_timeout(30);
	imp.set_write_timeout(30);
	imp.set_follow_location(true);
	duckdb_httplib_openssl::Headers imp_headers = {{"Authorization", "Bearer " + sts_parsed.access_token}};
	auto imp_response = imp.Post(imp_path.c_str(), imp_headers, imp_body, "application/json");
	if (!imp_response) {
		result.error = "no response from the service-account impersonation endpoint (" +
		                ext.service_account_impersonation_url + "); check network connectivity";
		return result;
	}
	if (imp_response->status != 200) {
		// The body names the missing role, which is the actual fix and is not
		// credential material -- it is an IAM policy statement about a
		// principal that just failed to use it.
		result.error = "service-account impersonation returned HTTP " +
		                std::to_string(imp_response->status) +
		                "\n\nThe federated principal needs roles/iam.workloadIdentityUser on the "
		                "service account it impersonates.";
		return result;
	}

	// A DIFFERENT response shape from the token endpoint: camelCase
	// `accessToken`, and `expireTime` as an RFC 3339 STRING rather than a
	// lifetime in seconds. Reusing ParseRefreshTokenResponse here would find
	// neither field and report "no access_token" for a successful call.
	picojson::value imp_root;
	if (!picojson::parse(imp_root, imp_response->body).empty() || !imp_root.is<picojson::object>()) {
		result.error = "the impersonation response was not valid JSON";
		return result;
	}
	const auto &imp_obj = imp_root.get<picojson::object>();
	auto token_it = imp_obj.find("accessToken");
	if (token_it == imp_obj.end() || !token_it->second.is<std::string>()) {
		result.error = "the impersonation response contained no accessToken";
		return result;
	}

	result.ok = true;
	result.access_token = token_it->second.get<std::string>();
	// Derived from the lifetime we REQUESTED rather than parsed from
	// `expireTime`: the requested value is what was granted, and adding an
	// RFC 3339 parser here would be a second thing to get wrong for no gain.
	result.expires_at_unix = now_unix + 3600;
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
	std::string contents, why;
	if (!ReadCredentialFile(path, contents, why)) {
		throw IOException(
		    "gdrive secret '%s': service-account key file " +
		        (why == "not found or not readable" ? why : "cannot be used, " + why) + ": '%s'. "
		    "Check the KEY_FILE path, or recreate the secret, e.g.:\n"
		    "  CREATE SECRET %s (TYPE gdrive, PROVIDER service_account, KEY_FILE '/etc/creds/sa.json');",
		    secret_name.c_str(), path.c_str(), secret_name.c_str());
	}
	return contents;
}

GDriveAuthContext BuildContextFromServiceAccount(const KeyValueSecret &kv, const std::string &secret_name,
                                                  int64_t max_age_seconds) {
	const std::string key_file = GetOrEmpty(kv, "key_file");
	if (key_file.empty()) {
		throw InvalidInputException("gdrive secret '%s' (PROVIDER service_account) is missing KEY_FILE.",
		                             secret_name.c_str());
	}
	std::string scope = GetOrEmpty(kv, "drive_scope");
	if (scope.empty()) {
		scope = SCOPE_DRIVE_READONLY; // REQ-NF-04
	}

	// Cache key covers the KEY FILE (path+size+mtime), the scope and the
	// delegated subject -- everything that changes which token Google hands
	// back. See FingerprintKey: a name-only key leaks tokens between
	// same-named secrets in different databases sharing one process.
	const std::string cache_key =
	    FingerprintKey(secret_name, "service_account",
	                   {KeyFileStamp(key_file), scope, GetOrEmpty(kv, "subject")});

	// The key is read and parsed HERE, at resolution, and the source keeps the
	// parsed key -- the same policy as credential_chain's captured ADC
	// document. The cache key and the handle's identity (which keys the path
	// and block caches) are stamped from this file now; a mint that re-read
	// the file an hour later could sign as a DIFFERENT principal replaced on
	// disk meanwhile and file its token, and its bytes, under the old
	// identity. A rotated key is picked up by the next open, whose new stamp
	// is a new identity. The cost: the key stays in memory for as long as a
	// handle holds the source, and every resolution reads the file once (a
	// local read; a cached token still needs no signing and no network call,
	// REQ-NF-02).
	std::string key_json = ReadFileOrThrow(key_file, secret_name);
	ServiceAccountKeyParse parsed = ParseServiceAccountKey(key_json);
	key_json.clear(); // the parsed key is what is kept; the raw text is not

	if (!parsed.ok) {
		// ParseServiceAccountKey guarantees parsed.error never quotes key
		// material (see its own doc comment and S-1.8's Catch2 coverage).
		throw InvalidInputException("gdrive secret '%s': invalid service-account key file '%s': %s",
		                             secret_name.c_str(), key_file.c_str(), parsed.error.c_str());
	}

	MintFn mint = [key = std::move(parsed.key), scope, secret_name]() {
		MintedToken minted = MintServiceAccountToken(key, scope, /*subject=*/"", NowUnix());
		if (!minted.ok) {
			throw IOException("gdrive secret '%s': failed to obtain an access token from Google: %s",
			                   secret_name.c_str(), minted.error.c_str());
		}
		return CachedTokenState {minted.access_token, minted.expires_at_unix, 0};
	};

	GDriveAuthContext ctx;
	ctx.tokens = std::make_shared<CachedTokenSource>(cache_key, std::move(mint), max_age_seconds);
	ctx.tokens->Token(); // eagerly: a bad key fails here, naming the secret
	ctx.drive_id = GetOrEmpty(kv, "drive_id");
	ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
	ctx.scope = scope;
	ctx.secret_name = secret_name;
	return ctx;
}

// ---------------------------------------------------------------------------
// PROVIDER credential_chain -- Application Default Credentials (D-10).
//
// Resolution is deliberately thin: find the ADC document, decide which of the
// two token strategies this extension ALREADY has applies to it, and hand
// off. An `authorized_user` document carries exactly the triple that the
// config provider's refresh path consumes; a `service_account` document is
// exactly what the RFC 7523 minting path consumes. No third strategy exists,
// and none should be added here.
//
// Everything about WHERE to look and WHAT a document means is pure and lives
// in gdrive_adc.cpp; this function only touches the environment, the
// filesystem, and the token cache.
// ---------------------------------------------------------------------------

//! Read an environment variable as a std::string, treating unset and empty
//! the same -- an exported-but-empty GOOGLE_APPLICATION_CREDENTIALS is a
//! shell accident, not a request to open a file called "".
std::string GetEnvOrEmpty(const char *name) {
	const char *value = std::getenv(name);
	return value ? std::string(value) : std::string();
}

//! The ADC file a credential_chain secret authenticates with. The
//! gdrive_adc_file setting outranks the environment: a DuckDB session cannot
//! export a variable to itself, so this is the only way to point one
//! connection at a specific credential. Empty if none is found.
std::string ResolveChainAdcPath(optional_ptr<FileOpener> opener) {
	Value setting;
	if (FileOpener::TryGetCurrentSetting(opener, "gdrive_adc_file", setting) && !setting.IsNull() &&
	    !setting.ToString().empty()) {
		return setting.ToString();
	}
	AdcPathInputs inputs;
	inputs.google_application_credentials = GetEnvOrEmpty("GOOGLE_APPLICATION_CREDENTIALS");
	inputs.cloudsdk_config = GetEnvOrEmpty("CLOUDSDK_CONFIG");
	inputs.home = GetEnvOrEmpty("HOME");
	inputs.appdata = GetEnvOrEmpty("APPDATA");
	return ResolveAdcPath(inputs);
}

GDriveAuthContext BuildContextFromCredentialChain(optional_ptr<FileOpener> opener, const KeyValueSecret &kv,
                                                   const std::string &secret_name, int64_t max_age_seconds) {
	std::string scope = GetOrEmpty(kv, "drive_scope");
	if (scope.empty()) {
		scope = SCOPE_DRIVE_READONLY; // REQ-NF-04
	}

	const std::string adc_path = ResolveChainAdcPath(opener);

	if (adc_path.empty()) {
		throw IOException("gdrive secret '%s': %s", secret_name.c_str(),
		                   NoCredentialsMessage().c_str());
	}

	std::string adc_json, why;
	if (!ReadCredentialFile(adc_path, adc_json, why)) {
		// The path is not secret, and naming it is what turns "no credentials"
		// from a riddle into a fact the reader can check with `ls`.
		throw IOException("gdrive secret '%s': no usable credentials file at '%s' (%s).\n\n%s",
		                   secret_name.c_str(), adc_path.c_str(), why.c_str(), NoCredentialsMessage().c_str());
	}

	AdcParse parsed = ParseAdcJson(adc_json);
	adc_json.clear(); // never keep credential text around longer than needed
	if (!parsed.ok) {
		// ParseAdcJson guarantees its error quotes no credential material.
		throw InvalidInputException("gdrive secret '%s': credentials file '%s' is unusable: %s",
		                             secret_name.c_str(), adc_path.c_str(), parsed.error.c_str());
	}

	// Checked before a token is minted, not on the first Drive call.
	//
	// Drive refuses a user credential that names no quota project outright,
	// and its message -- "the drive.googleapis.com API requires a quota
	// project, which is not set by default" -- arrives attached to whatever
	// query the user happened to run. It then reads as a problem with that
	// query, or with that file, rather than with a credential a gcloud command
	// wrote days earlier. Refusing here names the file and the fix, and avoids
	// minting a token that could not have worked.
	//
	// AUTHORIZED_USER only: a service account carries its own project
	// association, and sending it an x-goog-user-project it lacks
	// serviceusage permission on turns a working call into a 403.
	if (parsed.kind == AdcKind::AUTHORIZED_USER && parsed.user.quota_project_id.empty()) {
		throw IOException(
		    "gdrive secret '%s': the credentials at '%s' name no quota project, and Drive refuses a "
		    "user credential without one. Set it with:\n"
		    "  gcloud auth application-default set-quota-project <PROJECT_ID>\n"
		    "or use a service-account key, which carries its own project association\n"
		    "  CREATE SECRET (TYPE gdrive, PROVIDER service_account, KEY_FILE '/path/to/key.json');",
		    secret_name.c_str(), adc_path.c_str());
	}

	// The cache key fingerprints the RESOLVED SOURCE, not the provider name.
	// Keying on "credential_chain" alone would hand database A's token to
	// database B in the same process whenever both hold a chain secret and
	// point at different credentials -- the exact cross-tenant bug documented
	// at FingerprintKey above, which this provider would otherwise reintroduce
	// in its most tempting form (there are no user-supplied fields to key on).
	const std::string cache_key =
	    FingerprintKey(secret_name, "credential_chain", {KeyFileStamp(adc_path), scope});

	// The parsed document is captured, so a re-mint an hour into a scan does
	// not depend on the file still being where it was.
	MintFn mint = [parsed, adc_path, scope, secret_name]() {
		if (parsed.kind == AdcKind::EXTERNAL_ACCOUNT) {
			// Workload identity federation. No key is involved: the platform's
			// subject token is exchanged at STS and then used to impersonate a
			// service account WITH the Drive scope. The subject token is read
			// per exchange, so its hourly rotation is picked up.
			RefreshResult federated = ExchangeExternalAccountToken(parsed.external_account, scope, NowUnix());
			if (!federated.ok) {
				throw IOException("gdrive secret '%s': failed to obtain an access token from the "
				                   "workload-identity credentials at '%s': %s",
				                   secret_name.c_str(), adc_path.c_str(), federated.error.c_str());
			}
			return CachedTokenState {federated.access_token, federated.expires_at_unix, 0};
		}
		if (parsed.kind == AdcKind::AUTHORIZED_USER) {
			RefreshResult refreshed = RefreshUserToken(parsed.user.client_id, parsed.user.client_secret,
			                                            parsed.user.refresh_token, NowUnix());
			if (!refreshed.ok) {
				throw IOException("gdrive secret '%s': failed to refresh an access token from the "
				                   "credentials at '%s': %s\n\nIf this credential was revoked, re-run "
				                   "`gcloud auth application-default login`.",
				                   secret_name.c_str(), adc_path.c_str(), refreshed.error.c_str());
			}
			return CachedTokenState {refreshed.access_token, refreshed.expires_at_unix, 0};
		}
		// SERVICE_ACCOUNT. Note this is the arm google-cloud-cpp gets wrong for
		// Drive -- its token generator self-signs a JWT, which Drive rejects
		// with 401. Minting a real access token via RFC 7523 is what works, and
		// this extension already does it.
		MintedToken minted = MintServiceAccountToken(parsed.service_account, scope, /*subject=*/"", NowUnix());
		if (!minted.ok) {
			throw IOException("gdrive secret '%s': failed to obtain an access token using the "
			                   "service-account credentials at '%s': %s",
			                   secret_name.c_str(), adc_path.c_str(), minted.error.c_str());
		}
		return CachedTokenState {minted.access_token, minted.expires_at_unix, 0};
	};

	GDriveAuthContext ctx;
	ctx.tokens = std::make_shared<CachedTokenSource>(cache_key, std::move(mint), max_age_seconds);
	ctx.tokens->Token();
	ctx.drive_id = GetOrEmpty(kv, "drive_id");
	ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
	ctx.scope = scope;
	ctx.secret_name = secret_name;
	// User credentials only. A service account carries its own project
	// association, and sending it an x-goog-user-project it has no
	// serviceusage permission on turns a working call into a 403.
	if (parsed.kind == AdcKind::AUTHORIZED_USER) {
		ctx.quota_project_id = parsed.user.quota_project_id;
	}
	return ctx;
}

// ---------------------------------------------------------------------------
// PROVIDER authorization_code -- interactive browser consent (C-1..C-4).
//
// Runs at FIRST USE rather than at CREATE SECRET, so a SQL statement never
// blocks on a human clicking a consent screen, and so creating the secret is
// testable without a browser.
//
// After the flow completes the refresh token is written back into the secret,
// which is the difference between OAuth that works once and OAuth that keeps
// working. (The gsheets extension does not do this: it stores only the access
// token, so its browser flow has to be repeated every hour.)
// ---------------------------------------------------------------------------
//! The refresh grant as a mint function, for every provider holding a
//! refresh token: PROVIDER config and authorization_code.
MintFn RefreshGrantMint(const std::string &client_id, const std::string &client_secret,
                        const std::string &refresh_token, const std::string &secret_name) {
	return [client_id, client_secret, refresh_token, secret_name]() {
		RefreshResult refreshed = RefreshUserToken(client_id, client_secret, refresh_token, NowUnix());
		if (!refreshed.ok) {
			// REQ-NF-03: RefreshResult::error never contains the refresh token,
			// the client secret, or a raw echoed request/response body -- see
			// RefreshUserToken's own doc comment.
			throw IOException("gdrive secret '%s': failed to refresh an access token from Google: %s",
			                   secret_name.c_str(), refreshed.error.c_str());
		}
		return CachedTokenState {refreshed.access_token, refreshed.expires_at_unix, 0};
	};
}

GDriveAuthContext BuildContextFromAuthorizationCode(ClientContext &context, const KeyValueSecret &kv,
                                                     const std::string &secret_name, int64_t max_age_seconds) {
	std::string scope = GetOrEmpty(kv, "drive_scope");
	if (scope.empty()) {
		scope = SCOPE_DRIVE_READONLY;
	}

	const std::string client_id = GetOrEmpty(kv, "client_id");
	const std::string client_secret = GetOrEmpty(kv, "client_secret");
	if (client_id.empty() || client_secret.empty()) {
		throw InvalidInputException("gdrive secret '%s' (PROVIDER authorization_code) is missing CLIENT_ID or "
		                             "CLIENT_SECRET.",
		                             secret_name.c_str());
	}

	auto make_ctx = [&](std::shared_ptr<GDriveTokenSource> tokens) {
		GDriveAuthContext ctx;
		ctx.tokens = std::move(tokens);
		ctx.drive_id = GetOrEmpty(kv, "drive_id");
		ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
		ctx.scope = scope;
		ctx.secret_name = secret_name;
		return ctx;
	};

	// The refresh token names the USER: without it, a same-named secret
	// recreated with another user's refresh token was handed the first
	// user's cached access token for up to an hour.
	const std::string stored_refresh = GetOrEmpty(kv, "refresh_token");
	const std::string cache_key =
	    FingerprintKey(secret_name, "authorization_code", {client_id, client_secret, scope, stored_refresh});

	// A refresh token from an earlier consent means no browser is needed. This
	// is the normal path after the first use, and the reason the flow is worth
	// doing properly: consent happens once, not hourly.
	if (!stored_refresh.empty()) {
		auto tokens = std::make_shared<CachedTokenSource>(
		    cache_key, RefreshGrantMint(client_id, client_secret, stored_refresh, secret_name), max_age_seconds);
		try {
			tokens->Token();
			return make_ctx(std::move(tokens));
		} catch (const std::exception &) {
			// Fall through to a fresh consent: a revoked or expired refresh
			// token is exactly the case where re-consenting is the right
			// answer, and erroring out would leave the user to work that out
			// themselves.
		}
	} else {
		// No refresh token yet, but a consent earlier in this process may have
		// cached an access token. Without a refresh token it cannot be
		// renewed: once it lapses, Drive's 401 stands.
		auto tokens = std::make_shared<CachedTokenSource>(cache_key, MintFn(), max_age_seconds);
		std::lock_guard<std::mutex> lock(CacheMutex());
		auto it = TokenCache().find(cache_key);
		if (it != TokenCache().end() && TokenIsFresh(it->second, NowUnix(), max_age_seconds)) {
			return make_ctx(std::move(tokens));
		}
	}

	// Fail fast before anything is launched or bound. datazoo-oauth2's browser
	// launcher does no display check, so on a headless host the flow blocks
	// until the callback handler times out and then reports a timeout -- which
	// reads as "you were too slow" rather than "this machine has no browser".
	{
		DisplayEnvironment display_env;
#if !defined(_WIN32) && !defined(__APPLE__)
		display_env.is_posix_non_apple = true;
#endif
		display_env.display = GetEnvOrEmpty("DISPLAY");
		display_env.wayland_display = GetEnvOrEmpty("WAYLAND_DISPLAY");
		display_env.ssh_connection = GetEnvOrEmpty("SSH_CONNECTION");
		if (!CanLaunchBrowser(display_env)) {
			throw IOException("gdrive secret '%s': %s", secret_name.c_str(), NoBrowserMessage().c_str());
		}
	}

	GoogleOAuthParams google = BuildGoogleOAuthParams(scope);

	erpl_web::OAuth2Config config;
	config.client_id = client_id;
	config.client_secret = client_secret;
	config.scope = google.scope;
	// ALWAYS explicit -- OAuth2Config's defaults are SAP BTP-shaped. See
	// gdrive_oauth_params.hpp.
	config.custom_auth_url = google.auth_url;
	config.custom_token_url = google.token_url;
	// access_type=offline + prompt=consent; without both Google issues no
	// refresh token and the flow silently degrades to one hour of access.
	config.extra_auth_params = google.extra_auth_params;

	const std::string port = GetOrEmpty(kv, "redirect_port");
	config.redirect_uri = "http://localhost:" + (port.empty() ? std::string("8020") : port);

	erpl_web::OAuth2FlowV2 flow;
	erpl_web::OAuth2Tokens tokens;
	try {
		tokens = flow.ExecuteFlow(config);
	} catch (const std::exception &e) {
		throw IOException(
		    "gdrive secret '%s': the browser consent flow did not complete: %s\n"
		    "This flow needs a browser on this machine and a free port for the redirect "
		    "(%s). On a headless host use PROVIDER credential_chain or PROVIDER "
		    "service_account instead.",
		    secret_name.c_str(), e.what(), config.redirect_uri.c_str());
	}

	if (tokens.access_token.empty()) {
		throw IOException("gdrive secret '%s': the browser consent flow returned no access token.",
		                   secret_name.c_str());
	}

	// Persist the refresh token so this is the last browser prompt.
	//
	// This MUST go through the secret manager. Mutating the KeyValueSecret we
	// were handed does nothing: SecretMatch's constructor copies the
	// SecretEntry, whose copy constructor Clone()s the secret, so a lookup
	// returns a deep copy that is destroyed when the match goes out of scope.
	// Writing to it succeeds, changes nothing, and reports no error -- the
	// flow appears to work and the next expiry silently prompts again.
	if (!tokens.refresh_token.empty()) {
		erpl_web::OAuth2SecretTokenManager::UpdateSecretWithTokens(
		    context, &kv, tokens.access_token, tokens.expires_in > 0 ? tokens.expires_in : 3600,
		    tokens.refresh_token);
	}

	// Cache under the key the NEXT resolution will compute: it reads the
	// refresh token just persisted into the secret, not the one (absent or
	// revoked) this resolution started from. Keyed on the old one, the next
	// open missed this token and minted again, and the two keys split one
	// credential into two mint groups.
	const std::string flow_key =
	    tokens.refresh_token.empty()
	        ? cache_key
	        : FingerprintKey(secret_name, "authorization_code", {client_id, client_secret, scope, tokens.refresh_token});
	const int64_t expires_at = NowUnix() + (tokens.expires_in > 0 ? tokens.expires_in : 3600);
	{
		std::lock_guard<std::mutex> lock(CacheMutex());
		TokenCache()[flow_key] = CachedTokenState {tokens.access_token, expires_at, NowUnix()};
	}
	IncrementGlobalTokenRequest();

	// Later expiry or a 401 refreshes with the token the flow just returned;
	// without one (Google withheld it) there is nothing to refresh with.
	MintFn mint;
	if (!tokens.refresh_token.empty()) {
		mint = RefreshGrantMint(client_id, client_secret, tokens.refresh_token, secret_name);
	}
	return make_ctx(std::make_shared<CachedTokenSource>(flow_key, std::move(mint), max_age_seconds));
}

GDriveAuthContext BuildContextFromConfig(const KeyValueSecret &kv, const std::string &secret_name,
                                         int64_t max_age_seconds) {
	std::string scope = GetOrEmpty(kv, "drive_scope");
	if (scope.empty()) {
		scope = SCOPE_DRIVE_READONLY;
	}

	auto make_ctx = [&](std::shared_ptr<GDriveTokenSource> tokens) {
		GDriveAuthContext ctx;
		ctx.tokens = std::move(tokens);
		ctx.tokens->Token(); // eagerly: a bad refresh token fails here, naming the secret
		ctx.drive_id = GetOrEmpty(kv, "drive_id");
		ctx.root_folder_id = GetOrEmpty(kv, "root_folder_id");
		ctx.scope = scope;
		ctx.secret_name = secret_name;
		return ctx;
	};

	const std::string access_token = GetOrEmpty(kv, "access_token");
	const std::string refresh_token = GetOrEmpty(kv, "refresh_token");
	const std::string client_id = GetOrEmpty(kv, "client_id");
	const std::string client_secret = GetOrEmpty(kv, "client_secret");
	const bool can_refresh = !refresh_token.empty() && !client_id.empty() && !client_secret.empty();

	// An explicit ACCESS_TOKEN is used as-is: "I already have a token, use
	// it." With nothing else, the caller owns its lifetime and a 401 stands.
	// With REFRESH_TOKEN/CLIENT_ID/CLIENT_SECRET beside it, the pasted token is
	// still what is sent first -- but when Drive rejects it (expired, revoked)
	// the refresh grant takes over instead of failing an hour into a scan.
	if (!access_token.empty()) {
		if (!can_refresh) {
			return make_ctx(std::make_shared<StaticTokenSource>(access_token));
		}
		// The pasted token is part of the key: it is what this slot starts
		// from, and a refresh-only secret over the same credential must not be
		// handed it.
		const std::string cache_key =
		    FingerprintKey(secret_name, "config", {client_id, client_secret, refresh_token, access_token});
		return make_ctx(std::make_shared<CachedTokenSource>(
		    cache_key, RefreshGrantMint(client_id, client_secret, refresh_token, secret_name), max_age_seconds,
		    access_token));
	}

	// No ACCESS_TOKEN: mint from REFRESH_TOKEN + CLIENT_ID + CLIENT_SECRET via
	// `grant_type=refresh_token` against Google's token endpoint. This is the
	// delegated-user path a service account needs to write outside a Shared
	// Drive (it has no storage quota of its own -- see CLAUDE.md's
	// live-test-credentials note).
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

	// Keyed by the credential material, not the secret name -- see
	// FingerprintKey. Two databases in one process may both hold a secret
	// called "gdrive" pointing at different Google accounts.
	const std::string cache_key = FingerprintKey(secret_name, "config", {client_id, client_secret, refresh_token});
	return make_ctx(std::make_shared<CachedTokenSource>(
	    cache_key, RefreshGrantMint(client_id, client_secret, refresh_token, secret_name), max_age_seconds));
}

} // namespace

GDriveAuthContext GetAuthContext(optional_ptr<FileOpener> opener, const std::string &path) {
	// Through the opener, not a ClientContext: DuckDB opens an ATTACHed
	// database file with a database-level opener that has no client
	// connection, and secrets and settings are still reachable through it.
	auto secret_manager = FileOpener::TryGetSecretManager(opener);
	auto transaction = FileOpener::TryGetCatalogTransaction(opener);
	if (!secret_manager || !transaction) {
		throw IOException("gdrive: '%s' was opened without a database context, so no secret can be looked up", path);
	}
	auto match = secret_manager->LookupSecret(*transaction, path, "gdrive");

	if (!match.HasMatch()) {
		// A gdrive secret may EXIST and still not match: DuckDB's SCOPE clause
		// restricts which paths a secret applies to, and `SCOPE` is easy to
		// reach for when you meant `DRIVE_SCOPE`, the OAuth scope. Writing
		// SCOPE 'https://www.googleapis.com/auth/drive' scopes the secret to
		// paths beginning with that URL -- which no gdrive:// path does -- and
		// the secret then matches nothing. Telling that user "no secret
		// configured" sends them off to create a second one, which will not
		// match either. Say what actually happened.
		if (HasAnyGDriveSecret(opener)) {
			throw IOException(
			    "gdrive: a gdrive secret exists, but none of them apply to '%s'.\n"
			    "A secret's SCOPE clause limits which paths it covers. If you meant to set the "
			    "OAuth scope, the parameter is DRIVE_SCOPE -- SCOPE is DuckDB's path filter:\n"
			    "  CREATE SECRET g (TYPE gdrive, PROVIDER config, ACCESS_TOKEN '...',\n"
			    "                   DRIVE_SCOPE 'https://www.googleapis.com/auth/drive');\n"
			    "Check what is registered with: SELECT name, scope FROM duckdb_secrets();",
			    path);
		}
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

	// Cache identity: the secret's fields plus the credential actually used
	// (see gdrive_identity.hpp for the rules, which are unit-tested).
	std::vector<std::pair<std::string, std::string>> fields;
	for (const auto &entry : kv->secret_map) {
		fields.emplace_back(entry.first, entry.second.ToString());
	}
	std::string identity_source;
	if (base.GetProvider() == "service_account") {
		identity_source = KeyFileStamp(GetOrEmpty(*kv, "key_file"));
	} else if (base.GetProvider() == "credential_chain") {
		identity_source = KeyFileStamp(ResolveChainAdcPath(opener));
	}
	const std::string identity =
	    FingerprintKey(secret_name, base.GetProvider(), IdentityMaterial(base.GetProvider(), fields, identity_source));
	// Testing aid (gdrive_debug_token_max_age_seconds): trust a token for at
	// most this long, so expiry under a live handle is exercisable in seconds.
	// Validated here, where it is used: below 0 is an error, 1..60 is clamped
	// to 61 (any smaller cap makes every token stale on arrival -- one token
	// request per Drive request). See NormalizeMaxAgeSeconds.
	int64_t max_age_seconds = 0;
	Value max_age;
	if (FileOpener::TryGetCurrentSetting(opener, "gdrive_debug_token_max_age_seconds", max_age) &&
	    !max_age.IsNull() && !NormalizeMaxAgeSeconds(max_age.GetValue<int64_t>(), max_age_seconds)) {
		throw InvalidInputException("gdrive: gdrive_debug_token_max_age_seconds must be 0 (no cap) or positive, "
		                             "got %lld",
		                             static_cast<long long>(max_age.GetValue<int64_t>()));
	}
	auto with_identity = [&identity](GDriveAuthContext ctx) {
		ctx.identity = identity;
		return ctx;
	};

	if (base.GetProvider() == "service_account") {
		return with_identity(BuildContextFromServiceAccount(*kv, secret_name, max_age_seconds));
	}
	if (base.GetProvider() == "config") {
		return with_identity(BuildContextFromConfig(*kv, secret_name, max_age_seconds));
	}
	if (base.GetProvider() == "credential_chain") {
		return with_identity(BuildContextFromCredentialChain(opener, *kv, secret_name, max_age_seconds));
	}
	if (base.GetProvider() == "authorization_code") {
		// The one provider that needs a client connection: it writes refreshed
		// tokens back into the secret.
		auto client_context = FileOpener::TryGetClientContext(opener);
		if (!client_context) {
			throw IOException("gdrive secret '%s' (PROVIDER authorization_code) needs a client connection to "
			                  "refresh its tokens, and '%s' was opened without one (e.g. by ATTACH). Use a "
			                  "service_account, config or credential_chain secret for it.",
			                  secret_name.c_str(), path.c_str());
		}
		return with_identity(BuildContextFromAuthorizationCode(*client_context, *kv, secret_name, max_age_seconds));
	}

	throw InvalidInputException("gdrive secret '%s' has unsupported provider '%s'; expected 'service_account', "
	                             "'config', 'credential_chain' or 'authorization_code'.",
	                             secret_name.c_str(), base.GetProvider().c_str());
}

bool HasAnyGDriveSecret(optional_ptr<FileOpener> opener) {
	auto secret_manager = FileOpener::TryGetSecretManager(opener);
	auto transaction = FileOpener::TryGetCatalogTransaction(opener);
	if (!secret_manager || !transaction) {
		return false;
	}
	auto secrets = secret_manager->AllSecrets(*transaction);
	for (const auto &entry : secrets) {
		if (entry.secret && entry.secret->GetType() == "gdrive") {
			return true;
		}
	}
	return false;
}

} // namespace gdrive
} // namespace duckdb
