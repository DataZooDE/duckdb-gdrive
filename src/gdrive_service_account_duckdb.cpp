// S-1.7 -- the DuckDB-coupled half of the service-account JWT flow (RFC
// 7523): RS256-sign the assertion built by BuildAssertionParts (pure,
// gdrive_service_account_pure.cpp) and POST it to Google's token endpoint.
//
// Split rationale is in gdrive_service_account.hpp's header comment: claim
// construction is pure and unit-tested; signing needs OpenSSL and the token
// POST needs a socket, so both live here, exercised only by live SQL tests
// (CLAUDE.md, decision D-1 -- no mocks).
// NOMINMAX must precede every include: it only suppresses windows.h's min/max
// macros if nothing has pulled windows.h in yet, and its failure mode is
// silence on Windows only. Neither header below reaches windows.h today, so
// this was working -- but it was working by luck, and the same ordering in
// gdrive_auth.cpp stopped working the moment a header that does was added
// above it. Hoisted so luck is not load-bearing.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include "gdrive_service_account.hpp"

#include "duckdb/common/exception.hpp"

#include "httplib.hpp"

#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdint>
#include <memory>
#include <string>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Result of minting a token. Kept as a plain struct with primitive/std
// members (no DuckDB types) so gdrive_auth.cpp -- a different translation
// unit that does not include this file's header (there isn't one; this
// function is not part of the frozen gdrive_service_account.hpp contract) --
// can forward-declare the identical signature and call it directly.
// ---------------------------------------------------------------------------
struct MintedToken {
	bool ok = false;
	std::string error; //!< REQ-NF-03: never contains token or key material.
	std::string access_token;
	int64_t expires_at_unix = 0; //!< Absolute unix time the token stops being valid.
};

namespace {

// ---- OpenSSL RAII helpers, matching ../quack-oauth/src/jwt_verify.cpp -----

struct EvpPkeyDelete {
	void operator()(EVP_PKEY *p) const noexcept {
		EVP_PKEY_free(p);
	}
};
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDelete>;

struct EvpMdCtxDelete {
	void operator()(EVP_MD_CTX *p) const noexcept {
		EVP_MD_CTX_free(p);
	}
};
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDelete>;

struct BioDelete {
	void operator()(BIO *p) const noexcept {
		BIO_free(p);
	}
};
using BioPtr = std::unique_ptr<BIO, BioDelete>;

// Drains and discards OpenSSL's error queue. Deliberately does NOT forward
// OpenSSL's own error strings into any exception or MintedToken::error --
// on a key-shape mismatch those strings can echo fragments of DER/PEM
// structure, which is closer to key material than any error message should
// get (REQ-NF-03). Callers get a fixed, generic message instead.
void DrainOpenSslErrors() {
	while (ERR_get_error() != 0) {
	}
}

// RS256-sign `signing_input` with the service account's PEM private key.
// Returns the raw (binary) signature bytes, or std::string() on failure.
std::string SignRs256(const std::string &signing_input, const std::string &private_key_pem) {
	BioPtr key_bio(BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size())));
	if (!key_bio) {
		DrainOpenSslErrors();
		return {};
	}
	EvpPkeyPtr pkey(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr));
	if (!pkey) {
		DrainOpenSslErrors();
		return {};
	}

	EvpMdCtxPtr md_ctx(EVP_MD_CTX_new());
	if (!md_ctx) {
		DrainOpenSslErrors();
		return {};
	}
	if (EVP_DigestSignInit(md_ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1) {
		DrainOpenSslErrors();
		return {};
	}
	if (EVP_DigestSignUpdate(md_ctx.get(), signing_input.data(), signing_input.size()) != 1) {
		DrainOpenSslErrors();
		return {};
	}

	size_t sig_len = 0;
	if (EVP_DigestSignFinal(md_ctx.get(), nullptr, &sig_len) != 1) {
		DrainOpenSslErrors();
		return {};
	}
	std::string signature(sig_len, '\0');
	if (EVP_DigestSignFinal(md_ctx.get(), reinterpret_cast<unsigned char *>(&signature[0]), &sig_len) != 1) {
		DrainOpenSslErrors();
		return {};
	}
	signature.resize(sig_len);
	return signature;
}

// Splits "https://host[:port]/path" into scheme_host_port + path, the shape
// httplib::Client's constructor and Post() want separately.
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

// Extracts a single top-level string/number field from a JSON object body,
// without ever echoing the whole body (which, for an error response, is
// small and Google-authored, so that part is safe -- but callers should
// still not blindly forward arbitrary un-validated content upstream).
struct TokenResponse {
	bool parsed_ok = false;
	std::string access_token;
	int64_t expires_in = 0;
	std::string error;             // OAuth "error" field, e.g. "invalid_grant"
	std::string error_description; // OAuth "error_description" field
};

TokenResponse ParseTokenResponse(const std::string &body) {
	TokenResponse result;
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

} // namespace

// ---------------------------------------------------------------------------
// Mints a fresh access token for a service account: build the RFC 7523
// assertion (pure), sign it (above), POST it with
// grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer, and parse the
// response. Never throws -- failures come back in MintedToken::error so the
// caller (gdrive_auth.cpp) can wrap them in an actionable DuckDB exception
// naming the secret.
// ---------------------------------------------------------------------------
MintedToken MintServiceAccountToken(const ServiceAccountKey &key, const std::string &scope, const std::string &subject,
                                     int64_t now_unix) {
	MintedToken result;

	auto parts = BuildAssertionParts(key, scope, now_unix, /*lifetime_seconds=*/3600, subject);
	if (!parts.ok) {
		result.error = parts.error;
		return result;
	}

	std::string signature = SignRs256(parts.signing_input, key.private_key);
	if (signature.empty()) {
		result.error = "failed to RS256-sign the service-account assertion (check the private_key is a valid PEM RSA key)";
		return result;
	}

	std::string assertion = parts.signing_input + "." + Base64UrlEncode(signature);

	std::string token_uri = key.token_uri.empty() ? std::string(GOOGLE_TOKEN_URL) : key.token_uri;
	std::string scheme_host_port, path;
	if (!SplitUrl(token_uri, scheme_host_port, path)) {
		result.error = "service-account token_uri is not a valid URL";
		return result;
	}

	std::string body = "grant_type=" + UrlEncodeFormValue(JWT_BEARER_GRANT) + "&assertion=" + UrlEncodeFormValue(assertion);

	duckdb_httplib_openssl::Client client(scheme_host_port);
	client.set_connection_timeout(30);
	client.set_read_timeout(30);
	client.set_write_timeout(30);
	client.set_follow_location(true);

	auto response = client.Post(path.c_str(), body, "application/x-www-form-urlencoded");
	if (!response) {
		result.error = "no response from the Google token endpoint (" + token_uri + "); check network connectivity";
		return result;
	}

	auto parsed = ParseTokenResponse(response->body);
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
	// Google's default assertion lifetime is 3600s; trust expires_in when
	// present, otherwise fall back to the lifetime we requested.
	int64_t lifetime = parsed.expires_in > 0 ? parsed.expires_in : 3600;
	result.expires_at_unix = now_unix + lifetime;
	return result;
}

} // namespace gdrive
} // namespace duckdb
