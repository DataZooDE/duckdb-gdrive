#pragma once

#include <string>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Drive API error classification. PURE: no duckdb.hpp, no I/O.
//
// REQ-F-08 requires that not-found, permission-denied, quota-exceeded and
// expired-credential are distinguishable from one another. That is the whole
// point of this module: Drive returns 403 for at least three unrelated
// conditions, and collapsing them into "request failed" is precisely the
// failure mode risk R-2 predicts (a user hits a quota ceiling and reads it as
// a bug in the extension).
//
// The classifier keys off the machine-readable `reason` inside the JSON body,
// falling back to the HTTP status. Real captured response bodies live in
// test/cpp/testdata/drive_errors.json -- those are recorded data, not a mock:
// the classifier is a pure function and the fixtures are its inputs.
// ---------------------------------------------------------------------------

enum class GDriveErrorKind {
	//! 404. The file id or path does not exist, or is not visible to this
	//! identity -- Drive deliberately does not distinguish those two.
	NOT_FOUND,
	//! 401. Token missing, malformed, or expired. Actionable: re-auth.
	UNAUTHENTICATED,
	//! 403 insufficientPermissions / insufficientFilePermissions. The identity
	//! is known but the file's sharing does not allow this operation.
	PERMISSION_DENIED,
	//! 403 whose details[] carry reason "ACCESS_TOKEN_SCOPE_INSUFFICIENT". The
	//! identity is fine and the file may well be readable -- the TOKEN simply
	//! was not granted the Drive scope.
	//!
	//! Split out from PERMISSION_DENIED because Google gives both the same
	//! errors[0].reason ("insufficientPermissions"), so the generic message
	//! sends the reader to audit Drive sharing for a problem that is entirely
	//! in their credential. The common way to hit this is `gcloud auth
	//! application-default login` without --scopes, whose default
	//! (cloud-platform) does not include Drive.
	INSUFFICIENT_SCOPE,
	//! 403 storageQuotaExceeded and friends -- a *storage* limit, not a rate
	//! limit. Notably what a service account hits when writing outside a
	//! Shared Drive.
	STORAGE_QUOTA,
	//! 403 rateLimitExceeded / userRateLimitExceeded, and 429. A *rate* limit.
	//! Retryable after a delay. Must never read as a generic failure (R-2).
	RATE_LIMIT,
	//! 5xx. Retry with jittered backoff, then surface.
	TRANSIENT,
	//! 400. Malformed query or field selection -- our bug, not the user's.
	INVALID_REQUEST,
	//! 403 fileNotDownloadable / cannotDownloadAbusiveFile: a native Google
	//! format has no bytes, so alt=media cannot serve it. Signals the caller
	//! to fall back to files.export (REQ-F-07).
	NOT_DOWNLOADABLE,
	//! Anything unrecognised. Carries the raw message through.
	UNKNOWN,
};

struct GDriveError {
	GDriveErrorKind kind = GDriveErrorKind::UNKNOWN;
	int http_status = 0;
	//! Google's machine-readable `errors[0].reason`, e.g. "notFound".
	std::string reason;
	//! Google's human-readable `error.message`, verbatim.
	std::string message;
	//! From a Retry-After header when present, else 0. Only meaningful for
	//! RATE_LIMIT and TRANSIENT.
	int retry_after_seconds = 0;

	//! True when a retry could plausibly succeed without user action.
	bool IsRetryable() const;
};

//! Classify a Drive API failure.
//!
//! `body` may be empty or not be JSON at all (proxies and load balancers
//! return HTML); classification must then fall back to the status code
//! without throwing.
GDriveError ClassifyDriveError(int http_status, const std::string &body,
                               const std::string &retry_after_header = "");

//! Build the message the user actually sees.
//!
//! `context` names what was being attempted in the user's own terms -- the
//! gdrive:// path, not a file id they have never seen. REQ-NF-03: this must
//! never interpolate a token, a key, or an Authorization header, and there is
//! a test asserting exactly that.
std::string FormatUserMessage(const GDriveError &error, const std::string &context);

//! Which DuckDB exception the caller should throw. Kept as a plain enum so
//! this stays pure; the DuckDB layer switches on it.
enum class GDriveExceptionType {
	IO,
	PERMISSION,
	INVALID_INPUT,
};

GDriveExceptionType ExceptionTypeFor(GDriveErrorKind kind);

} // namespace gdrive
} // namespace duckdb
