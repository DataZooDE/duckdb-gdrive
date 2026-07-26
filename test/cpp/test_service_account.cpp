// S-1.5 / S-1.6 -- RFC 7523 service-account JWT assertion, pure halves.
// Pure logic only: no DuckDB linkage, no network, no clock (issued_at is a
// parameter). Signing (RS256) and the token POST are a different slice.
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

#include "gdrive_service_account.hpp"

using duckdb::gdrive::AssertionParts;
using duckdb::gdrive::Base64UrlEncode;
using duckdb::gdrive::BuildAssertionParts;
using duckdb::gdrive::GOOGLE_TOKEN_URL;
using duckdb::gdrive::ParseServiceAccountKey;
using duckdb::gdrive::ServiceAccountKey;
using duckdb::gdrive::ServiceAccountKeyParse;

namespace {

std::string ReadTestFile(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	REQUIRE(f.good());
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// The throwaway fixture lives beside this test file. CTest / the ninja build
// runs the binary from the build directory, so resolve relative to the
// source tree via the compiled-in path fed by CMake would be nicer, but this
// binary has no such define; tests are run from the repo root per the task's
// documented command, so a repo-root-relative path works.
const std::string kFixturePath = "test/cpp/testdata/fake_sa_key.json";

// NOTE on numeric claims (iat/exp): whether picojson stores a bare integer
// literal as int64_t or double depends on whether PICOJSON_USE_INT64 was
// defined before picojson.h's *first* inclusion in this translation unit --
// and that's a link-wide ODR hazard, not something this file fully controls,
// since src/gdrive_errors.cpp (a different slice) also includes picojson.h
// without defining it. So claims here are compared via get<double>() rather
// than get<int64_t>(): iat/exp are unix-second timestamps (~1.7e9), far
// inside double's exact-integer range (2^53), so there is no precision risk.
picojson::object ParseObjectOrFail(const std::string &json_text) {
	picojson::value v;
	std::string err = picojson::parse(v, json_text);
	REQUIRE(err.empty());
	REQUIRE(v.is<picojson::object>());
	return v.get<picojson::object>();
}

std::string Serialize(const picojson::object &obj) {
	return picojson::value(obj).serialize();
}

} // namespace

// ---------------------------------------------------------------------------
// Base64UrlEncode -- RFC 4648 section 5 vectors, plus the classic failure
// modes: padding boundaries (len 0,1,2,3), '+' / '/' substitution, no '='.
// ---------------------------------------------------------------------------

TEST_CASE("Base64UrlEncode RFC 4648 test vectors", "[sa][base64]") {
	REQUIRE(Base64UrlEncode("") == "");
	REQUIRE(Base64UrlEncode("f") == "Zg");
	REQUIRE(Base64UrlEncode("fo") == "Zm8");
	REQUIRE(Base64UrlEncode("foo") == "Zm9v");
	REQUIRE(Base64UrlEncode("foob") == "Zm9vYg");
	REQUIRE(Base64UrlEncode("fooba") == "Zm9vYmE");
	REQUIRE(Base64UrlEncode("foobar") == "Zm9vYmFy");
}

TEST_CASE("Base64UrlEncode never emits padding", "[sa][base64]") {
	for (const std::string &s : {std::string(""), std::string("f"), std::string("fo"), std::string("foo"),
	                              std::string("foob"), std::string("fooba"), std::string("foobar")}) {
		auto encoded = Base64UrlEncode(s);
		REQUIRE(encoded.find('=') == std::string::npos);
	}
}

TEST_CASE("Base64UrlEncode substitutes the URL-safe alphabet characters", "[sa][base64]") {
	// Standard base64 of these three bytes (0xFB 0xFF 0xBF) is "+/+/" in
	// stock base64; the URL-safe alphabet must turn that into "-_-_".
	const std::string data {static_cast<char>(0xFB), static_cast<char>(0xFF), static_cast<char>(0xBF),
	                         static_cast<char>(0xFB), static_cast<char>(0xFF), static_cast<char>(0xBF)};
	auto encoded = Base64UrlEncode(data);
	REQUIRE(encoded.find('+') == std::string::npos);
	REQUIRE(encoded.find('/') == std::string::npos);
	REQUIRE(encoded.find('-') != std::string::npos);
	REQUIRE(encoded.find('_') != std::string::npos);
}

TEST_CASE("Base64UrlEncode handles high-byte binary data", "[sa][base64]") {
	std::string data;
	for (int i = 0; i < 256; ++i) {
		data.push_back(static_cast<char>(i));
	}
	auto encoded = Base64UrlEncode(data);
	REQUIRE_FALSE(encoded.empty());
	// Decode it back with a minimal, independent decoder to prove round-trip.
	auto value_of = [](unsigned char c) -> int {
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		if (c == '-')
			return 62;
		if (c == '_')
			return 63;
		return -1;
	};
	std::string decoded;
	unsigned int buffer = 0;
	int bits = 0;
	for (unsigned char c : encoded) {
		int v = value_of(c);
		REQUIRE(v >= 0);
		buffer = (buffer << 6) | static_cast<unsigned int>(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
		}
	}
	REQUIRE(decoded == data);
}

// ---------------------------------------------------------------------------
// BuildAssertionParts -- byte-exact signing_input for fixed inputs.
// ---------------------------------------------------------------------------

TEST_CASE("BuildAssertionParts produces the exact expected header and claims", "[sa][assertion]") {
	ServiceAccountKey key;
	key.client_email = "test-sa@test-project.iam.gserviceaccount.com";
	key.private_key = "unused-by-this-function";
	key.private_key_id = "testkeyid123";
	key.token_uri = GOOGLE_TOKEN_URL;
	key.project_id = "test-project";

	const int64_t issued_at = 1700000000;
	const int lifetime = 3600;
	const std::string scope = "https://www.googleapis.com/auth/drive.readonly";

	auto parts = BuildAssertionParts(key, scope, issued_at, lifetime);
	REQUIRE(parts.ok);
	REQUIRE(parts.error.empty());

	// Decoded header must carry alg/typ/kid.
	auto header = ParseObjectOrFail(parts.header_json);
	REQUIRE(header.at("alg").get<std::string>() == "RS256");
	REQUIRE(header.at("typ").get<std::string>() == "JWT");
	REQUIRE(header.at("kid").get<std::string>() == "testkeyid123");

	// Decoded claims must carry iss, scope, aud, exp, iat -- and no sub.
	auto claims = ParseObjectOrFail(parts.claims_json);
	REQUIRE(claims.at("iss").get<std::string>() == key.client_email);
	REQUIRE(claims.at("scope").get<std::string>() == scope);
	REQUIRE(claims.at("aud").get<std::string>() == GOOGLE_TOKEN_URL);
	REQUIRE(claims.at("iat").get<double>() == static_cast<double>(issued_at));
	REQUIRE(claims.at("exp").get<double>() == static_cast<double>(issued_at + lifetime));
	REQUIRE(claims.find("sub") == claims.end());

	// signing_input is base64url(header) . base64url(claims), byte-exact.
	const std::string expected = Base64UrlEncode(parts.header_json) + "." + Base64UrlEncode(parts.claims_json);
	REQUIRE(parts.signing_input == expected);

	// And the two halves decode back to exactly what we asserted above --
	// i.e. header_json/claims_json really are what got signed, not a
	// diagnostics-only side channel that drifted from the real thing.
	auto dot = parts.signing_input.find('.');
	REQUIRE(dot != std::string::npos);
}

TEST_CASE("BuildAssertionParts: subject present adds sub; absent omits it entirely", "[sa][assertion][sub]") {
	ServiceAccountKey key;
	key.client_email = "svc@proj.iam.gserviceaccount.com";
	key.private_key = "x";
	key.token_uri = GOOGLE_TOKEN_URL;

	auto without_sub = BuildAssertionParts(key, "scope-a", 1000, 3600, "");
	REQUIRE(without_sub.ok);
	auto claims_no_sub = ParseObjectOrFail(without_sub.claims_json);
	REQUIRE(claims_no_sub.find("sub") == claims_no_sub.end());
	// Also assert at the raw-string level: the literal substring "sub" must
	// not appear as a JSON key when no subject was given.
	REQUIRE(without_sub.claims_json.find("\"sub\"") == std::string::npos);

	auto with_sub = BuildAssertionParts(key, "scope-a", 1000, 3600, "user@example.com");
	REQUIRE(with_sub.ok);
	auto claims_sub = ParseObjectOrFail(with_sub.claims_json);
	REQUIRE(claims_sub.at("sub").get<std::string>() == "user@example.com");
}

TEST_CASE("BuildAssertionParts: exp = issued_at + lifetime_seconds", "[sa][assertion][exp]") {
	ServiceAccountKey key;
	key.client_email = "svc@proj.iam.gserviceaccount.com";
	key.private_key = "x";
	key.token_uri = GOOGLE_TOKEN_URL;

	auto parts = BuildAssertionParts(key, "scope-a", 5000, 120);
	REQUIRE(parts.ok);
	auto claims = ParseObjectOrFail(parts.claims_json);
	REQUIRE(claims.at("iat").get<double>() == 5000.0);
	REQUIRE(claims.at("exp").get<double>() == 5120.0);
}

TEST_CASE("BuildAssertionParts: lifetime_seconds above Google's 3600s cap is clamped, not rejected",
          "[sa][assertion][lifetime_cap]") {
	// Decision (documented in gdrive_service_account_pure.cpp): silently clamp
	// to Google's enforced ceiling rather than fail the request outright --
	// Google's own token endpoint would reject/ignore an over-long lifetime
	// anyway, and a hard error here would be a surprising way to find that
	// out for what is otherwise a working configuration.
	ServiceAccountKey key;
	key.client_email = "svc@proj.iam.gserviceaccount.com";
	key.private_key = "x";
	key.token_uri = GOOGLE_TOKEN_URL;

	auto parts = BuildAssertionParts(key, "scope-a", 1000, 7200 /* > 3600 */);
	REQUIRE(parts.ok);
	auto claims = ParseObjectOrFail(parts.claims_json);
	REQUIRE(claims.at("exp").get<double>() == 1000.0 + 3600.0);
}

TEST_CASE("BuildAssertionParts: non-positive lifetime is rejected", "[sa][assertion][lifetime_cap]") {
	ServiceAccountKey key;
	key.client_email = "svc@proj.iam.gserviceaccount.com";
	key.private_key = "x";
	key.token_uri = GOOGLE_TOKEN_URL;

	auto zero = BuildAssertionParts(key, "scope-a", 1000, 0);
	REQUIRE_FALSE(zero.ok);
	REQUIRE_FALSE(zero.error.empty());

	auto negative = BuildAssertionParts(key, "scope-a", 1000, -5);
	REQUIRE_FALSE(negative.ok);
	REQUIRE_FALSE(negative.error.empty());
}

TEST_CASE("BuildAssertionParts: a quote and a backslash in scope do not break the claims JSON",
          "[sa][assertion][escaping]") {
	ServiceAccountKey key;
	key.client_email = "svc@proj.iam.gserviceaccount.com";
	key.private_key = "x";
	key.token_uri = GOOGLE_TOKEN_URL;

	const std::string tricky_scope = "weird\"scope\\with/quote";
	auto parts = BuildAssertionParts(key, tricky_scope, 1000, 3600);
	REQUIRE(parts.ok);
	auto claims = ParseObjectOrFail(parts.claims_json); // throws / fails REQUIRE if JSON is broken
	REQUIRE(claims.at("scope").get<std::string>() == tricky_scope);
}

TEST_CASE("BuildAssertionParts: token_uri (aud) defaults to GOOGLE_TOKEN_URL when the key has none",
          "[sa][assertion][default_aud]") {
	ServiceAccountKey key;
	key.client_email = "svc@proj.iam.gserviceaccount.com";
	key.private_key = "x";
	key.token_uri = ""; // not set

	auto parts = BuildAssertionParts(key, "scope-a", 1000, 3600);
	REQUIRE(parts.ok);
	auto claims = ParseObjectOrFail(parts.claims_json);
	REQUIRE(claims.at("aud").get<std::string>() == GOOGLE_TOKEN_URL);
}

TEST_CASE("BuildAssertionParts rejects an empty scope", "[sa][assertion][reject]") {
	ServiceAccountKey key;
	key.client_email = "svc@proj.iam.gserviceaccount.com";
	key.private_key = "x";

	auto parts = BuildAssertionParts(key, "", 1000, 3600);
	REQUIRE_FALSE(parts.ok);
	REQUIRE_FALSE(parts.error.empty());
}

// ---------------------------------------------------------------------------
// ParseServiceAccountKey -- happy path + token_uri defaulting.
// ---------------------------------------------------------------------------

TEST_CASE("ParseServiceAccountKey parses a well-formed key and defaults token_uri", "[sa][parse]") {
	auto doc = ReadTestFile(kFixturePath);
	auto result = ParseServiceAccountKey(doc);
	REQUIRE(result.ok);
	REQUIRE(result.error.empty());
	REQUIRE(result.key.client_email == "fake-sa@fake-project-id.iam.gserviceaccount.com");
	REQUIRE_FALSE(result.key.private_key.empty());
	REQUIRE(result.key.private_key.find("-----BEGIN") != std::string::npos);
	REQUIRE(result.key.private_key_id == "fakekeyid0123456789abcdef0123456789abcd");
	REQUIRE(result.key.project_id == "fake-project-id");
	// The fixture DOES set token_uri explicitly; prove the default kicks in
	// only when it's genuinely absent.
	REQUIRE(result.key.token_uri == "https://oauth2.googleapis.com/token");
}

TEST_CASE("ParseServiceAccountKey defaults token_uri to GOOGLE_TOKEN_URL when absent", "[sa][parse][default_uri]") {
	auto obj = ParseObjectOrFail(ReadTestFile(kFixturePath));
	obj.erase("token_uri");
	auto result = ParseServiceAccountKey(Serialize(obj));
	REQUIRE(result.ok);
	REQUIRE(result.key.token_uri == GOOGLE_TOKEN_URL);
}

// ---------------------------------------------------------------------------
// ParseServiceAccountKey -- rejections, each with a DISTINCT message.
// ---------------------------------------------------------------------------

TEST_CASE("ParseServiceAccountKey rejects input that is not JSON at all", "[sa][parse][reject]") {
	auto result = ParseServiceAccountKey("this is not { json at all");
	REQUIRE_FALSE(result.ok);
	REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("ParseServiceAccountKey rejects valid JSON that is not an object", "[sa][parse][reject]") {
	auto array_result = ParseServiceAccountKey("[1, 2, 3]");
	REQUIRE_FALSE(array_result.ok);
	REQUIRE_FALSE(array_result.error.empty());

	auto string_result = ParseServiceAccountKey("\"just a string\"");
	REQUIRE_FALSE(string_result.ok);
	REQUIRE_FALSE(string_result.error.empty());
}

TEST_CASE("ParseServiceAccountKey rejects a document missing client_email", "[sa][parse][reject]") {
	auto obj = ParseObjectOrFail(ReadTestFile(kFixturePath));
	obj.erase("client_email");
	auto result = ParseServiceAccountKey(Serialize(obj));
	REQUIRE_FALSE(result.ok);
	REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("ParseServiceAccountKey rejects a document missing private_key", "[sa][parse][reject]") {
	auto obj = ParseObjectOrFail(ReadTestFile(kFixturePath));
	obj.erase("private_key");
	auto result = ParseServiceAccountKey(Serialize(obj));
	REQUIRE_FALSE(result.ok);
	REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("ParseServiceAccountKey rejects type != service_account and names the actual type",
          "[sa][parse][reject][type]") {
	auto obj = ParseObjectOrFail(ReadTestFile(kFixturePath));
	obj["type"] = picojson::value(std::string("authorized_user"));
	auto result = ParseServiceAccountKey(Serialize(obj));
	REQUIRE_FALSE(result.ok);
	REQUIRE_FALSE(result.error.empty());
	CAPTURE(result.error);
	REQUIRE(result.error.find("authorized_user") != std::string::npos);
}

TEST_CASE("ParseServiceAccountKey specifically detects an OAuth *client* JSON and says so",
          "[sa][parse][reject][oauth_client_json]") {
	// Google's OAuth client-secret download has an "installed" or "web"
	// top-level wrapper and no "private_key" -- a genuinely common mistake
	// (grabbing the wrong download from the Cloud Console) that deserves its
	// own actionable message rather than a generic "missing private_key".
	const std::string oauth_client_json = R"({
	  "installed": {
	    "client_id": "123-abc.apps.googleusercontent.com",
	    "project_id": "some-project",
	    "auth_uri": "https://accounts.google.com/o/oauth2/auth",
	    "token_uri": "https://oauth2.googleapis.com/token",
	    "client_secret": "not-a-real-secret",
	    "redirect_uris": ["http://localhost"]
	  }
	})";
	auto result = ParseServiceAccountKey(oauth_client_json);
	REQUIRE_FALSE(result.ok);
	REQUIRE_FALSE(result.error.empty());
	CAPTURE(result.error);
	auto lower = result.error;
	for (auto &c : lower)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	REQUIRE(lower.find("oauth") != std::string::npos);
}

TEST_CASE("All ParseServiceAccountKey rejection messages are pairwise distinct", "[sa][parse][reject][distinct]") {
	auto base = ParseObjectOrFail(ReadTestFile(kFixturePath));

	auto missing_email = base;
	missing_email.erase("client_email");

	auto missing_key = base;
	missing_key.erase("private_key");

	auto wrong_type = base;
	wrong_type["type"] = picojson::value(std::string("authorized_user"));

	std::vector<std::string> messages;
	messages.push_back(ParseServiceAccountKey("not json").error);
	messages.push_back(ParseServiceAccountKey("[1,2,3]").error);
	messages.push_back(ParseServiceAccountKey(Serialize(missing_email)).error);
	messages.push_back(ParseServiceAccountKey(Serialize(missing_key)).error);
	messages.push_back(ParseServiceAccountKey(Serialize(wrong_type)).error);

	for (size_t i = 0; i < messages.size(); ++i) {
		REQUIRE_FALSE(messages[i].empty());
		for (size_t j = i + 1; j < messages.size(); ++j) {
			CAPTURE(i, j, messages[i], messages[j]);
			REQUIRE(messages[i] != messages[j]);
		}
	}
}

// ---------------------------------------------------------------------------
// REQ-NF-03 -- no error message may ever contain any part of the private key.
//
// Feed ParseServiceAccountKey documents where the private_key IS present but
// something ELSE is broken, and assert the private key material never shows
// up in the returned error string.
// ---------------------------------------------------------------------------

TEST_CASE("REQ-NF-03: error messages never contain the private key material", "[sa][parse][security][req-nf-03]") {
	auto base = ParseObjectOrFail(ReadTestFile(kFixturePath));
	REQUIRE(base.at("private_key").is<std::string>());
	const std::string private_key_pem = base.at("private_key").get<std::string>();
	REQUIRE(private_key_pem.find("-----BEGIN") != std::string::npos);

	// Take a real, non-trivial substring of the key body (skip the PEM
	// banner itself so we're really checking key *material*, not the
	// boilerplate marker text).
	auto body_start = private_key_pem.find("-----\n");
	REQUIRE(body_start != std::string::npos);
	body_start += 6;
	const std::string key_fragment = private_key_pem.substr(body_start, 40);
	REQUIRE(key_fragment.size() == 40);

	auto CheckNoLeak = [&](const ServiceAccountKeyParse &result) {
		REQUIRE_FALSE(result.ok);
		CAPTURE(result.error);
		REQUIRE(result.error.find(key_fragment) == std::string::npos);
		REQUIRE(result.error.find("-----BEGIN") == std::string::npos);
		REQUIRE(result.error.find(private_key_pem) == std::string::npos);
	};

	// Case 1: private_key present and intact, but client_email missing.
	{
		auto doc = base;
		doc.erase("client_email");
		CheckNoLeak(ParseServiceAccountKey(Serialize(doc)));
	}

	// Case 2: private_key present and intact, but type is wrong.
	{
		auto doc = base;
		doc["type"] = picojson::value(std::string("authorized_user"));
		CheckNoLeak(ParseServiceAccountKey(Serialize(doc)));
	}

	// Case 3: private_key present, but the document is truncated garbage
	// starting right after the private_key field -- a broken-JSON case that
	// still carries the full key material in the input text.
	{
		auto full = ReadTestFile(kFixturePath);
		auto cut_point = full.find("\"client_email\"");
		REQUIRE(cut_point != std::string::npos);
		auto truncated = full.substr(0, cut_point) + "\"client_em"; // deliberately broken JSON
		REQUIRE(truncated.find("-----BEGIN") != std::string::npos); // sanity: key is really in there
		auto result = ParseServiceAccountKey(truncated);
		REQUIRE_FALSE(result.ok);
		CAPTURE(result.error);
		REQUIRE(result.error.find(key_fragment) == std::string::npos);
		REQUIRE(result.error.find("-----BEGIN") == std::string::npos);
	}
}
