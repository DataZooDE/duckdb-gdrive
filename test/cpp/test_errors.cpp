// S-2.3 -- Drive error classification. Pure logic, no DuckDB linkage.
//
// REQ-F-08: not-found, permission-denied, quota-exceeded and expired-
// credential must be distinguishable. R-2: a quota error must not read as a
// generic failure. REQ-NF-03: no credential ever appears in a message.
//
// Fixtures split into two groups:
//   - CAPTURED: verbatim bodies from test/cpp/testdata/drive_errors.json,
//     recorded against the real Google Drive API. Embedded as literals here
//     (not JSON-parsed from the file) so the test cannot silently drift from
//     what was actually seen on the wire, and to avoid a JSON dependency in
//     the test itself.
//   - DOCUMENTATION-DERIVED: shapes we could not provoke live (Shared Drive
//     permission errors, an actually-exhausted per-user rate limit, a 429).
//     Built from Google's published Drive API error reference. Flagged
//     explicitly; if real bodies are captured later these should be swapped
//     in and this comment updated.
#include <catch2/catch_test_macros.hpp>

#include "gdrive_errors.hpp"

using duckdb::gdrive::ClassifyDriveError;
using duckdb::gdrive::FormatUserMessage;
using duckdb::gdrive::ExceptionTypeFor;
using duckdb::gdrive::GDriveError;
using duckdb::gdrive::GDriveErrorKind;
using duckdb::gdrive::GDriveExceptionType;

namespace {

// ---------------------------------------------------------------------------
// CAPTURED bodies -- verbatim from test/cpp/testdata/drive_errors.json
// ---------------------------------------------------------------------------

const char *kNotFoundGet = R"({
  "error": {
    "code": 404,
    "message": "File not found: 1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.",
    "errors": [
      {
        "message": "File not found: 1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.",
        "domain": "global",
        "reason": "notFound",
        "location": "fileId",
        "locationType": "parameter"
      }
    ]
  }
})";

const char *kUnauthorized = R"({
  "error": {
    "code": 401,
    "message": "Request had invalid authentication credentials. Expected OAuth 2 access token, login cookie or other valid authentication credential. See https://developers.google.com/identity/sign-in/web/devconsole-project.",
    "errors": [
      {
        "message": "Invalid Credentials",
        "domain": "global",
        "reason": "authError",
        "location": "Authorization",
        "locationType": "header"
      }
    ],
    "status": "UNAUTHENTICATED"
  }
})";

const char *kNoAuth = R"({
  "error": {
    "code": 401,
    "message": "Request is missing required authentication credential. Expected OAuth 2 access token, login cookie or other valid authentication credential. See https://developers.google.com/identity/sign-in/web/devconsole-project.",
    "errors": [
      {
        "message": "Login Required.",
        "domain": "global",
        "reason": "required",
        "location": "Authorization",
        "locationType": "header"
      }
    ],
    "status": "UNAUTHENTICATED",
    "details": [
      {
        "@type": "type.googleapis.com/google.rpc.ErrorInfo",
        "reason": "CREDENTIALS_MISSING",
        "domain": "googleapis.com",
        "metadata": {
          "service": "drive.googleapis.com",
          "method": "google.apps.drive.v3.DriveAbout.Get"
        }
      }
    ]
  }
})";

const char *kBadField = R"({
  "error": {
    "code": 400,
    "message": "Invalid field selection nonexistentfield",
    "errors": [
      {
        "message": "Invalid field selection nonexistentfield",
        "domain": "global",
        "reason": "invalidParameter",
        "location": "fields",
        "locationType": "parameter"
      }
    ]
  }
})";

const char *kBadQuery = R"({
  "error": {
    "code": 400,
    "message": "Invalid Value",
    "errors": [
      {
        "message": "Invalid Value",
        "domain": "global",
        "reason": "invalid",
        "location": "q",
        "locationType": "parameter"
      }
    ]
  }
})";

const char *kStorageQuota = R"({
  "error": {
    "code": 403,
    "message": "Service Accounts do not have storage quota. Leverage shared drives (https://developers.google.com/workspace/drive/api/guides/about-shareddrives), or use OAuth delegation (http://support.google.com/a/answer/7281227) instead.",
    "errors": [
      {
        "message": "Service Accounts do not have storage quota. Leverage shared drives (https://developers.google.com/workspace/drive/api/guides/about-shareddrives), or use OAuth delegation (http://support.google.com/a/answer/7281227) instead.",
        "domain": "usageLimits",
        "reason": "storageQuotaExceeded"
      }
    ]
  }
})";

// ---------------------------------------------------------------------------
// DOCUMENTATION-DERIVED bodies -- not yet captured live. Shapes taken from
// Google's published Drive API error reference
// (https://developers.google.com/drive/api/guides/handle-errors).
// ---------------------------------------------------------------------------

const char *kInsufficientPermissions = R"({
  "error": {
    "code": 403,
    "message": "The user does not have sufficient permissions for file 1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.",
    "errors": [
      {
        "message": "The user does not have sufficient permissions for file 1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.",
        "domain": "global",
        "reason": "insufficientFilePermissions"
      }
    ]
  }
})";

const char *kRateLimitExceeded = R"({
  "error": {
    "code": 403,
    "message": "User Rate Limit Exceeded",
    "errors": [
      {
        "message": "User Rate Limit Exceeded",
        "domain": "usageLimits",
        "reason": "userRateLimitExceeded"
      }
    ]
  }
})";

const char *kFileNotDownloadable = R"({
  "error": {
    "code": 403,
    "message": "Only files with binary content can be downloaded. Use Export with Docs Editors files.",
    "errors": [
      {
        "message": "Only files with binary content can be downloaded. Use Export with Docs Editors files.",
        "domain": "global",
        "reason": "fileNotDownloadable"
      }
    ]
  }
})";

const char *kAbusiveFile = R"({
  "error": {
    "code": 403,
    "message": "This file has been identified as malware or spyware and cannot be downloaded.",
    "errors": [
      {
        "message": "This file has been identified as malware or spyware and cannot be downloaded.",
        "domain": "global",
        "reason": "cannotDownloadAbusiveFile"
      }
    ]
  }
})";

const char *kServerError5xx = R"({
  "error": {
    "code": 500,
    "message": "Internal error encountered.",
    "errors": [
      {
        "message": "Internal error encountered.",
        "domain": "global",
        "reason": "internalError"
      }
    ]
  }
})";

} // namespace

// ---------------------------------------------------------------------------
// NOT_FOUND -- captured
// ---------------------------------------------------------------------------

TEST_CASE("404 notFound classifies as NOT_FOUND", "[errors]") {
	auto err = ClassifyDriveError(404, kNotFoundGet);
	REQUIRE(err.kind == GDriveErrorKind::NOT_FOUND);
	REQUIRE(err.http_status == 404);
	REQUIRE(err.reason == "notFound");
}

TEST_CASE("404 on alt=media (export_non_native / not_found_media) classifies as NOT_FOUND", "[errors]") {
	auto err = ClassifyDriveError(404, kNotFoundGet);
	REQUIRE(err.kind == GDriveErrorKind::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// UNAUTHENTICATED -- captured
// ---------------------------------------------------------------------------

TEST_CASE("401 authError classifies as UNAUTHENTICATED", "[errors]") {
	auto err = ClassifyDriveError(401, kUnauthorized);
	REQUIRE(err.kind == GDriveErrorKind::UNAUTHENTICATED);
	REQUIRE(err.http_status == 401);
}

TEST_CASE("401 missing credential (no_auth) classifies as UNAUTHENTICATED", "[errors]") {
	auto err = ClassifyDriveError(401, kNoAuth);
	REQUIRE(err.kind == GDriveErrorKind::UNAUTHENTICATED);
}

// ---------------------------------------------------------------------------
// INVALID_REQUEST -- captured
// ---------------------------------------------------------------------------

TEST_CASE("400 invalidParameter (bad_field) classifies as INVALID_REQUEST", "[errors]") {
	auto err = ClassifyDriveError(400, kBadField);
	REQUIRE(err.kind == GDriveErrorKind::INVALID_REQUEST);
}

TEST_CASE("400 invalid query (bad_query) classifies as INVALID_REQUEST", "[errors]") {
	auto err = ClassifyDriveError(400, kBadQuery);
	REQUIRE(err.kind == GDriveErrorKind::INVALID_REQUEST);
}

// ---------------------------------------------------------------------------
// STORAGE_QUOTA -- captured. The whole point of the module: this is a 403
// with domain "usageLimits" but reason "storageQuotaExceeded" -- a storage
// ceiling, categorically NOT a rate limit, and must not collapse into one.
// ---------------------------------------------------------------------------

TEST_CASE("403 storageQuotaExceeded classifies as STORAGE_QUOTA, not RATE_LIMIT", "[errors]") {
	auto err = ClassifyDriveError(403, kStorageQuota);
	REQUIRE(err.kind == GDriveErrorKind::STORAGE_QUOTA);
	REQUIRE(err.kind != GDriveErrorKind::RATE_LIMIT);
	REQUIRE(err.reason == "storageQuotaExceeded");
}

TEST_CASE("STORAGE_QUOTA user message names the service-account storage cause, not a rate limit", "[errors]") {
	auto err = ClassifyDriveError(403, kStorageQuota);
	auto msg = FormatUserMessage(err, "gdrive://Finance/2026/actuals.parquet");
	REQUIRE(msg.find("quota") != std::string::npos);
	// Must not be phrased as a rate/retry problem.
	REQUIRE(msg.find("rate limit") == std::string::npos);
	REQUIRE(msg.find("Retry-After") == std::string::npos);
}

// ---------------------------------------------------------------------------
// PERMISSION_DENIED -- documentation-derived
// ---------------------------------------------------------------------------

TEST_CASE("403 insufficientFilePermissions classifies as PERMISSION_DENIED (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(403, kInsufficientPermissions);
	REQUIRE(err.kind == GDriveErrorKind::PERMISSION_DENIED);
}

TEST_CASE("403 insufficientPermissions reason string also maps to PERMISSION_DENIED (doc-derived)", "[errors]") {
	const char *body = R"({
	  "error": {
	    "code": 403,
	    "message": "Insufficient Permission",
	    "errors": [ { "message": "Insufficient Permission", "domain": "global", "reason": "insufficientPermissions" } ]
	  }
	})";
	auto err = ClassifyDriveError(403, body);
	REQUIRE(err.kind == GDriveErrorKind::PERMISSION_DENIED);
}

// ---------------------------------------------------------------------------
// RATE_LIMIT -- documentation-derived
// ---------------------------------------------------------------------------

TEST_CASE("403 userRateLimitExceeded classifies as RATE_LIMIT (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(403, kRateLimitExceeded);
	REQUIRE(err.kind == GDriveErrorKind::RATE_LIMIT);
}

TEST_CASE("403 rateLimitExceeded reason string also maps to RATE_LIMIT (doc-derived)", "[errors]") {
	const char *body = R"({
	  "error": {
	    "code": 403,
	    "message": "Rate Limit Exceeded",
	    "errors": [ { "message": "Rate Limit Exceeded", "domain": "usageLimits", "reason": "rateLimitExceeded" } ]
	  }
	})";
	auto err = ClassifyDriveError(403, body);
	REQUIRE(err.kind == GDriveErrorKind::RATE_LIMIT);
}

TEST_CASE("429 with a body classifies as RATE_LIMIT regardless of body content (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(429, kRateLimitExceeded);
	REQUIRE(err.kind == GDriveErrorKind::RATE_LIMIT);
}

TEST_CASE("429 with an empty body classifies as RATE_LIMIT (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(429, "");
	REQUIRE(err.kind == GDriveErrorKind::RATE_LIMIT);
}

TEST_CASE("429 with no body at all (empty string, no retry-after) classifies as RATE_LIMIT", "[errors]") {
	auto err = ClassifyDriveError(429, "", "");
	REQUIRE(err.kind == GDriveErrorKind::RATE_LIMIT);
	REQUIRE(err.IsRetryable());
}

TEST_CASE("RATE_LIMIT user message names the Drive API quota explicitly (R-2)", "[errors]") {
	auto err = ClassifyDriveError(429, kRateLimitExceeded, "30");
	auto msg = FormatUserMessage(err, "gdrive://Finance/2026/actuals.parquet");
	REQUIRE(msg.find("Drive API") != std::string::npos);
	REQUIRE(msg.find("quota") != std::string::npos);
	// Retry delay, when known, must be mentioned literally.
	REQUIRE(msg.find("30") != std::string::npos);
}

TEST_CASE("RATE_LIMIT user message still mentions quota when retry-after is unknown (R-2)", "[errors]") {
	auto err = ClassifyDriveError(429, "");
	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("Drive API") != std::string::npos);
	REQUIRE(msg.find("quota") != std::string::npos);
}

// ---------------------------------------------------------------------------
// NOT_DOWNLOADABLE -- documentation-derived
// ---------------------------------------------------------------------------

TEST_CASE("403 fileNotDownloadable classifies as NOT_DOWNLOADABLE (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(403, kFileNotDownloadable);
	REQUIRE(err.kind == GDriveErrorKind::NOT_DOWNLOADABLE);
}

TEST_CASE("403 cannotDownloadAbusiveFile classifies as NOT_DOWNLOADABLE (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(403, kAbusiveFile);
	REQUIRE(err.kind == GDriveErrorKind::NOT_DOWNLOADABLE);
}

// ---------------------------------------------------------------------------
// TRANSIENT -- documentation-derived
// ---------------------------------------------------------------------------

TEST_CASE("500 classifies as TRANSIENT (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(500, kServerError5xx);
	REQUIRE(err.kind == GDriveErrorKind::TRANSIENT);
	REQUIRE(err.IsRetryable());
}

TEST_CASE("502/503/504 with empty or HTML bodies all classify as TRANSIENT", "[errors]") {
	REQUIRE(ClassifyDriveError(502, "").kind == GDriveErrorKind::TRANSIENT);
	REQUIRE(ClassifyDriveError(503, "<html><body>Bad Gateway</body></html>").kind == GDriveErrorKind::TRANSIENT);
	REQUIRE(ClassifyDriveError(504, "Gateway Timeout").kind == GDriveErrorKind::TRANSIENT);
}

// ---------------------------------------------------------------------------
// Never throws -- empty, truncated, HTML bodies (proxies/load balancers)
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyDriveError never throws on an empty body", "[errors]") {
	REQUIRE_NOTHROW(ClassifyDriveError(404, ""));
	REQUIRE_NOTHROW(ClassifyDriveError(500, ""));
	REQUIRE_NOTHROW(ClassifyDriveError(200, ""));
}

TEST_CASE("ClassifyDriveError never throws on a truncated / malformed JSON body", "[errors]") {
	REQUIRE_NOTHROW(ClassifyDriveError(500, "{\"error\": {\"code\": 500, \"mess"));
	REQUIRE_NOTHROW(ClassifyDriveError(403, "{"));
	REQUIRE_NOTHROW(ClassifyDriveError(403, "not json at all"));
}

TEST_CASE("ClassifyDriveError never throws on an HTML body from a proxy/load balancer", "[errors]") {
	const char *html = "<html><head><title>502 Bad Gateway</title></head>"
	                   "<body><center>502 Bad Gateway</center></body></html>";
	GDriveError err;
	REQUIRE_NOTHROW(err = ClassifyDriveError(502, html));
	REQUIRE(err.kind == GDriveErrorKind::TRANSIENT);
	REQUIRE(err.http_status == 502);
}

TEST_CASE("Unrecognised status/body falls back to UNKNOWN without throwing", "[errors]") {
	GDriveError err;
	REQUIRE_NOTHROW(err = ClassifyDriveError(999, "{\"surprise\": true}"));
	REQUIRE(err.kind == GDriveErrorKind::UNKNOWN);
}

TEST_CASE("FormatUserMessage never throws, even on a default-constructed error", "[errors]") {
	GDriveError err;
	REQUIRE_NOTHROW(FormatUserMessage(err, "gdrive://x"));
}

// ---------------------------------------------------------------------------
// REQ-NF-03 -- no credential ever appears in a message.
//
// Decision: FormatUserMessage never interpolates Google's raw `message` for
// UNAUTHENTICATED (the case most likely to carry back a header/token in a
// diagnostic echo) -- it substitutes a fixed, generic re-auth instruction
// instead. For all other kinds the upstream message is scanned defensively:
// even though Drive's documented shapes for those kinds don't carry secrets,
// classification runs on untrusted response bodies, so the safety property
// must not depend on Google never changing its error text. Any bearer-token-
// shaped (`ya29.`, `Bearer `) or PEM-block-shaped substring is redacted
// before being folded into the user-facing string.
// ---------------------------------------------------------------------------

TEST_CASE("FormatUserMessage never leaks an access-token-shaped string for UNAUTHENTICATED", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNAUTHENTICATED;
	err.http_status = 401;
	err.reason = "authError";
	err.message = "Invalid Credentials: token ya29.a0ARrdaM-secret rejected";

	auto msg = FormatUserMessage(err, "gdrive://Finance/2026/actuals.parquet");
	REQUIRE(msg.find("ya29") == std::string::npos);
	REQUIRE(msg.find("secret") == std::string::npos);
}

TEST_CASE("FormatUserMessage never leaks a Bearer header echoed into the message", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNAUTHENTICATED;
	err.http_status = 401;
	err.reason = "authError";
	err.message = "Authorization: Bearer abc123.def456.ghi789 was rejected by the server";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("Bearer abc123") == std::string::npos);
	REQUIRE(msg.find("abc123.def456.ghi789") == std::string::npos);
}

TEST_CASE("FormatUserMessage never leaks a PEM private-key block", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.reason = "unknown";
	err.message = "bad request, saw key -----BEGIN PRIVATE KEY-----\n"
	              "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQ\n"
	              "-----END PRIVATE KEY-----\n in payload";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("BEGIN PRIVATE KEY") == std::string::npos);
	REQUIRE(msg.find("MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQ") == std::string::npos);
}

TEST_CASE("FormatUserMessage on the real captured unauthorized body carries no header/credential text", "[errors][security]") {
	auto err = ClassifyDriveError(401, kUnauthorized);
	auto msg = FormatUserMessage(err, "gdrive://Finance/2026/actuals.parquet");
	REQUIRE(msg.find("Authorization") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Adversarial bypasses found in codex review 2026-07-26 (wave 0), plus
// further variants judged realistic for an untrusted Drive response body.
// One test per bypass; each asserts the exact secret substring is absent.
// ---------------------------------------------------------------------------

TEST_CASE("FormatUserMessage never leaks a Google refresh token (1// prefix)", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "refresh token 1//0gREAL_SECRET rejected";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("1//0gREAL_SECRET") == std::string::npos);
	REQUIRE(msg.find("REAL_SECRET") == std::string::npos);
}

TEST_CASE("FormatUserMessage redacts Bearer case-insensitively with a tab separator", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "saw header bearer\tabc.def in request";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("abc.def") == std::string::npos);
}

TEST_CASE("FormatUserMessage redacts Bearer with multiple spaces, mixed case", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "BEARER    superSecretToken123 was invalid";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("superSecretToken123") == std::string::npos);
}

TEST_CASE("FormatUserMessage redacts a JSON-ish access_token value", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "upstream body was {\"access_token\": \"ya29-should-not-matter-SECRETVALUE\"}";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("SECRETVALUE") == std::string::npos);
}

TEST_CASE("FormatUserMessage redacts a JSON-ish refresh_token value even without the 1// prefix", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "{\"refresh_token\":\"OPAQUE_REFRESH_SECRET\"}";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("OPAQUE_REFRESH_SECRET") == std::string::npos);
}

TEST_CASE("FormatUserMessage redacts a JSON-ish private_key value", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "config had \"private_key\": \"c3VwZXJTZWNyZXRLZXlNYXRlcmlhbA==\" embedded";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("c3VwZXJTZWNyZXRLZXlNYXRlcmlhbA==") == std::string::npos);
}

TEST_CASE("FormatUserMessage redacts a JSON-ish client_secret value", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "oauth exchange failed, client_secret=GOCSPX-superSecretValue rejected";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("GOCSPX-superSecretValue") == std::string::npos);
}

TEST_CASE("FormatUserMessage redacts a JSON-ish id_token value", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "{\"id_token\": \"eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.SECRET_SIG\"}";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("SECRET_SIG") == std::string::npos);
}

TEST_CASE("FormatUserMessage does NOT redact client_id -- it is not a secret", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "client_id=123456-abcdef.apps.googleusercontent.com is unrecognised";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("123456-abcdef.apps.googleusercontent.com") != std::string::npos);
}

TEST_CASE("FormatUserMessage redacts an access_token in a URL query string", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "request to https://example.com/callback?access_token=QUERY_STRING_SECRET&state=xyz failed";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("QUERY_STRING_SECRET") == std::string::npos);
	// The unrelated 'state' query param must survive -- over-redaction is not free.
	REQUIRE(msg.find("state=xyz") != std::string::npos);
}

TEST_CASE("FormatUserMessage redacts an Authorization: Basic header value", "[errors][security]") {
	GDriveError err;
	err.kind = GDriveErrorKind::UNKNOWN;
	err.http_status = 400;
	err.message = "rejected header Authorization: Basic dXNlcjpTZWNyZXRQYXNzd29yZA== from client";

	auto msg = FormatUserMessage(err, "gdrive://x");
	REQUIRE(msg.find("dXNlcjpTZWNyZXRQYXNzd29yZA==") == std::string::npos);
}

// ---------------------------------------------------------------------------
// FormatUserMessage must include `context` -- the user's gdrive:// path, not
// only a file id they have never seen.
// ---------------------------------------------------------------------------

TEST_CASE("FormatUserMessage for NOT_FOUND names the resolved gdrive:// path", "[errors]") {
	auto err = ClassifyDriveError(404, kNotFoundGet);
	auto msg = FormatUserMessage(err, "gdrive://Finance/2026/actuals.parquet");
	REQUIRE(msg.find("gdrive://Finance/2026/actuals.parquet") != std::string::npos);
}

TEST_CASE("FormatUserMessage for PERMISSION_DENIED names the context path (doc-derived)", "[errors]") {
	auto err = ClassifyDriveError(403, kInsufficientPermissions);
	auto msg = FormatUserMessage(err, "gdrive://Shared/report.csv");
	REQUIRE(msg.find("gdrive://Shared/report.csv") != std::string::npos);
}

TEST_CASE("FormatUserMessage for TRANSIENT names the context path", "[errors]") {
	auto err = ClassifyDriveError(500, kServerError5xx);
	auto msg = FormatUserMessage(err, "gdrive://a/b.csv");
	REQUIRE(msg.find("gdrive://a/b.csv") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Retry-After header parsing
// ---------------------------------------------------------------------------

TEST_CASE("Retry-After as plain seconds is parsed", "[errors]") {
	auto err = ClassifyDriveError(429, "", "120");
	REQUIRE(err.retry_after_seconds == 120);
}

TEST_CASE("Retry-After absent defaults retry_after_seconds to 0", "[errors]") {
	auto err = ClassifyDriveError(429, "");
	REQUIRE(err.retry_after_seconds == 0);
}

TEST_CASE("Retry-After as an HTTP date does not throw and yields a non-negative value", "[errors]") {
	// Decision: HTTP-date Retry-After values are not parsed into a duration;
	// 0 (unknown) is acceptable per the header contract. What matters is that
	// this never throws and never produces a negative/garbage value.
	GDriveError err;
	REQUIRE_NOTHROW(err = ClassifyDriveError(429, "", "Wed, 21 Oct 2026 07:28:00 GMT"));
	REQUIRE(err.retry_after_seconds == 0);
}

TEST_CASE("Retry-After garbage string does not throw and yields 0", "[errors]") {
	GDriveError err;
	REQUIRE_NOTHROW(err = ClassifyDriveError(429, "", "not-a-number"));
	REQUIRE(err.retry_after_seconds == 0);
}

// ---------------------------------------------------------------------------
// IsRetryable
// ---------------------------------------------------------------------------

TEST_CASE("IsRetryable is true for RATE_LIMIT and TRANSIENT, false for the rest", "[errors]") {
	GDriveError rate;
	rate.kind = GDriveErrorKind::RATE_LIMIT;
	REQUIRE(rate.IsRetryable());

	GDriveError transient;
	transient.kind = GDriveErrorKind::TRANSIENT;
	REQUIRE(transient.IsRetryable());

	GDriveError not_found;
	not_found.kind = GDriveErrorKind::NOT_FOUND;
	REQUIRE_FALSE(not_found.IsRetryable());

	GDriveError unauth;
	unauth.kind = GDriveErrorKind::UNAUTHENTICATED;
	REQUIRE_FALSE(unauth.IsRetryable());

	GDriveError perm;
	perm.kind = GDriveErrorKind::PERMISSION_DENIED;
	REQUIRE_FALSE(perm.IsRetryable());

	GDriveError storage;
	storage.kind = GDriveErrorKind::STORAGE_QUOTA;
	REQUIRE_FALSE(storage.IsRetryable());

	GDriveError invalid;
	invalid.kind = GDriveErrorKind::INVALID_REQUEST;
	REQUIRE_FALSE(invalid.IsRetryable());

	GDriveError not_downloadable;
	not_downloadable.kind = GDriveErrorKind::NOT_DOWNLOADABLE;
	REQUIRE_FALSE(not_downloadable.IsRetryable());

	GDriveError unknown;
	unknown.kind = GDriveErrorKind::UNKNOWN;
	REQUIRE_FALSE(unknown.IsRetryable());
}

// ---------------------------------------------------------------------------
// ExceptionTypeFor -- maps kinds onto the DuckDB exception the caller throws
// ---------------------------------------------------------------------------

TEST_CASE("ExceptionTypeFor maps NOT_FOUND, RATE_LIMIT, STORAGE_QUOTA, TRANSIENT to IO", "[errors]") {
	REQUIRE(ExceptionTypeFor(GDriveErrorKind::NOT_FOUND) == GDriveExceptionType::IO);
	REQUIRE(ExceptionTypeFor(GDriveErrorKind::RATE_LIMIT) == GDriveExceptionType::IO);
	REQUIRE(ExceptionTypeFor(GDriveErrorKind::STORAGE_QUOTA) == GDriveExceptionType::IO);
	REQUIRE(ExceptionTypeFor(GDriveErrorKind::TRANSIENT) == GDriveExceptionType::IO);
	REQUIRE(ExceptionTypeFor(GDriveErrorKind::UNAUTHENTICATED) == GDriveExceptionType::IO);
}

TEST_CASE("ExceptionTypeFor maps PERMISSION_DENIED to PERMISSION", "[errors]") {
	REQUIRE(ExceptionTypeFor(GDriveErrorKind::PERMISSION_DENIED) == GDriveExceptionType::PERMISSION);
}

TEST_CASE("ExceptionTypeFor maps INVALID_REQUEST to INVALID_INPUT", "[errors]") {
	REQUIRE(ExceptionTypeFor(GDriveErrorKind::INVALID_REQUEST) == GDriveExceptionType::INVALID_INPUT);
}

// ---------------------------------------------------------------------------
// UNAUTHENTICATED message content -- names the secret / re-auth need, per HLD
// section 9 ("names the secret and that it needs re-auth").
// ---------------------------------------------------------------------------

TEST_CASE("UNAUTHENTICATED user message tells the user to re-authenticate", "[errors]") {
	auto err = ClassifyDriveError(401, kUnauthorized);
	auto msg = FormatUserMessage(err, "gdrive://Finance/2026/actuals.parquet");
	REQUIRE((msg.find("re-auth") != std::string::npos || msg.find("authenticat") != std::string::npos));
}

// ---------------------------------------------------------------------------
// NOT_FOUND message content -- HLD section 9: "no such file", naming path.
// ---------------------------------------------------------------------------

TEST_CASE("NOT_FOUND user message says no such file", "[errors]") {
	auto err = ClassifyDriveError(404, kNotFoundGet);
	auto msg = FormatUserMessage(err, "gdrive://x/y.csv");
	REQUIRE(msg.find("no such file") != std::string::npos);
}
