#pragma once

#include <string>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Service-account JWT flow, RFC 7523. The one genuinely new piece of auth
// code in this project (HLD section 5.3).
//
// Google's server-to-server flow is NOT Microsoft's client-credentials POST.
// It signs a JWT assertion with the service account's RSA private key and
// posts it with grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer.
//
// This header is split so the claim construction is PURE (testable against
// fixed inputs with no network and no clock) while signing and the token POST
// live in the DuckDB-side translation unit.
// ---------------------------------------------------------------------------

constexpr const char *GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr const char *GOOGLE_AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char *JWT_BEARER_GRANT = "urn:ietf:params:oauth:grant-type:jwt-bearer";

//! Drive scopes, narrowest first (REQ-NF-04).
constexpr const char *SCOPE_DRIVE_READONLY = "https://www.googleapis.com/auth/drive.readonly";
constexpr const char *SCOPE_DRIVE_FILE = "https://www.googleapis.com/auth/drive.file";
constexpr const char *SCOPE_DRIVE = "https://www.googleapis.com/auth/drive";

//! The fields we need out of a service-account key JSON. Deliberately does not
//! keep the whole document: less surface for a private key to leak through.
struct ServiceAccountKey {
	std::string client_email;
	std::string private_key; //!< PEM. NEVER log, never put in an error message.
	std::string private_key_id;
	std::string token_uri; //!< Defaults to GOOGLE_TOKEN_URL when absent.
	std::string project_id;
};

struct ServiceAccountKeyParse {
	bool ok = false;
	std::string error; //!< Must never quote the key material.
	ServiceAccountKey key;
};

//! Parse a service-account key JSON document.
//!
//! Rejects, with a distinct message for each (slice S-1.8): not JSON; missing
//! client_email; missing private_key; a `type` other than "service_account"
//! (a common mistake is handing it an OAuth *client* JSON instead).
//!
//! REQ-NF-03: no error message produced here may contain any part of
//! `private_key`, and there is a test asserting it.
ServiceAccountKeyParse ParseServiceAccountKey(const std::string &json_text);

//! The unsigned halves of the assertion: base64url(header) + "." +
//! base64url(claims).
//!
//! Split out from signing so it can be asserted byte-for-byte against fixed
//! inputs -- `issued_at` is a parameter rather than a call to the clock
//! precisely so the test is deterministic.
//!
//! Claims per RFC 7523 + Google's requirements:
//!   iss = client_email, scope, aud = token_uri,
//!   exp = issued_at + lifetime_seconds (Google caps this at 3600),
//!   iat = issued_at, and `sub` only when impersonating a user via
//!   domain-wide delegation.
struct AssertionParts {
	bool ok = false;
	std::string error;
	std::string signing_input; //!< header.claims, base64url, ready to sign
	std::string header_json;   //!< decoded, for tests and diagnostics
	std::string claims_json;   //!< decoded, for tests and diagnostics
};

AssertionParts BuildAssertionParts(const ServiceAccountKey &key,
                                   const std::string &scope,
                                   int64_t issued_at,
                                   int lifetime_seconds = 3600,
                                   const std::string &subject = "");

//! base64url without padding, per RFC 7515. Exposed because JWT encoding is
//! famously the place this gets wrong (standard base64, or padding left on)
//! and it deserves its own test.
std::string Base64UrlEncode(const std::string &data);

} // namespace gdrive
} // namespace duckdb
