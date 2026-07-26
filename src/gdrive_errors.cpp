// S-2.3 -- Drive error classification. PURE: no duckdb.hpp, no I/O.
//
// See src/include/gdrive_errors.hpp for the contract and rationale.
#include "gdrive_errors.hpp"

#include <picojson/picojson.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace duckdb {
namespace gdrive {

namespace {

// ---------------------------------------------------------------------------
// Best-effort extraction of Google's `error.errors[0].reason` and
// `error.message` from a Drive API JSON error body. Never throws: picojson's
// ::parse reports errors via an out-param string, and any shape mismatch
// (missing keys, wrong types, HTML instead of JSON) is treated the same way
// -- both fields come back empty and classification falls back to the HTTP
// status alone.
// ---------------------------------------------------------------------------
struct ParsedBody {
	std::string reason;
	std::string message;
	bool ok = false;
};

ParsedBody ParseErrorBody(const std::string &body) {
	ParsedBody out;
	if (body.empty()) {
		return out;
	}

	picojson::value root;
	std::string parse_err = picojson::parse(root, body);
	if (!parse_err.empty() || !root.is<picojson::object>()) {
		return out;
	}

	const auto &top = root.get<picojson::object>();
	auto error_it = top.find("error");
	if (error_it == top.end() || !error_it->second.is<picojson::object>()) {
		return out;
	}
	const auto &error_obj = error_it->second.get<picojson::object>();

	auto message_it = error_obj.find("message");
	if (message_it != error_obj.end() && message_it->second.is<std::string>()) {
		out.message = message_it->second.get<std::string>();
	}

	auto errors_it = error_obj.find("errors");
	if (errors_it != error_obj.end() && errors_it->second.is<picojson::array>()) {
		const auto &errors_arr = errors_it->second.get<picojson::array>();
		if (!errors_arr.empty() && errors_arr[0].is<picojson::object>()) {
			const auto &first = errors_arr[0].get<picojson::object>();
			auto reason_it = first.find("reason");
			if (reason_it != first.end() && reason_it->second.is<std::string>()) {
				out.reason = reason_it->second.get<std::string>();
			}
		}
	}

	out.ok = true;
	return out;
}

//! Parses a Retry-After header. Per RFC 9110 this is either a non-negative
//! integer number of seconds, or an HTTP-date. We handle the seconds form;
//! for an HTTP-date we deliberately return 0 (unknown) rather than parsing a
//! calendar date -- the caller still knows the request is retryable, just not
//! precisely when, and 0 never causes a caller to wait *less* than it should
//! by more than an immediate-retry margin. Never throws: strtol fails soft.
int ParseRetryAfterSeconds(const std::string &header) {
	if (header.empty()) {
		return 0;
	}
	// Must be all digits (optionally with leading/trailing whitespace) to be
	// treated as the seconds form; anything else (an HTTP-date, garbage) is 0.
	std::string trimmed = header;
	size_t begin = trimmed.find_first_not_of(" \t");
	size_t end = trimmed.find_last_not_of(" \t");
	if (begin == std::string::npos) {
		return 0;
	}
	trimmed = trimmed.substr(begin, end - begin + 1);
	if (trimmed.empty() || !std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char c) { return std::isdigit(c); })) {
		return 0;
	}
	errno = 0;
	char *endptr = nullptr;
	long value = std::strtol(trimmed.c_str(), &endptr, 10);
	if (endptr == trimmed.c_str() || value < 0) {
		return 0;
	}
	return static_cast<int>(value);
}

GDriveErrorKind ClassifyFromReason(const std::string &reason) {
	if (reason == "notFound") {
		return GDriveErrorKind::NOT_FOUND;
	}
	if (reason == "authError" || reason == "required") {
		return GDriveErrorKind::UNAUTHENTICATED;
	}
	if (reason == "insufficientPermissions" || reason == "insufficientFilePermissions") {
		return GDriveErrorKind::PERMISSION_DENIED;
	}
	if (reason == "storageQuotaExceeded") {
		return GDriveErrorKind::STORAGE_QUOTA;
	}
	if (reason == "rateLimitExceeded" || reason == "userRateLimitExceeded") {
		return GDriveErrorKind::RATE_LIMIT;
	}
	if (reason == "fileNotDownloadable" || reason == "cannotDownloadAbusiveFile") {
		return GDriveErrorKind::NOT_DOWNLOADABLE;
	}
	if (reason == "invalidParameter" || reason == "invalid") {
		return GDriveErrorKind::INVALID_REQUEST;
	}
	return GDriveErrorKind::UNKNOWN;
}

GDriveErrorKind ClassifyFromStatus(int http_status) {
	if (http_status == 404) {
		return GDriveErrorKind::NOT_FOUND;
	}
	if (http_status == 401) {
		return GDriveErrorKind::UNAUTHENTICATED;
	}
	if (http_status == 400) {
		return GDriveErrorKind::INVALID_REQUEST;
	}
	if (http_status == 429) {
		return GDriveErrorKind::RATE_LIMIT;
	}
	if (http_status >= 500 && http_status < 600) {
		return GDriveErrorKind::TRANSIENT;
	}
	return GDriveErrorKind::UNKNOWN;
}

// ---------------------------------------------------------------------------
// REQ-NF-03 -- redact anything credential-shaped before it can reach a
// user-facing message. Applied defensively to any upstream `message` text
// that gets folded into FormatUserMessage's output, not just the
// UNAUTHENTICATED path: classification runs on untrusted response bodies, so
// this must not depend on Google's error text never changing shape.
//
// Heuristics, deliberately simple and over-inclusive (a false positive here
// just means a slightly more generic message, which is the safe direction):
//   - "Bearer <token>"                      -> "Bearer [redacted]"
//   - an OAuth access-token-shaped run      -> "[redacted]"
//     starting "ya29." (Google's own prefix for user access tokens)
//   - a PEM block (BEGIN/END ... KEY)       -> "[redacted]"
// ---------------------------------------------------------------------------
std::string RedactCredentials(const std::string &input) {
	std::string out = input;

	// PEM blocks: drop everything from "-----BEGIN" to the matching "-----END
	// ...-----" (or to end of string if unterminated).
	for (;;) {
		size_t begin_pos = out.find("-----BEGIN");
		if (begin_pos == std::string::npos) {
			break;
		}
		size_t end_marker = out.find("-----END", begin_pos);
		size_t end_pos;
		if (end_marker == std::string::npos) {
			end_pos = out.size();
		} else {
			size_t trailer = out.find("-----", end_marker + 8);
			end_pos = (trailer == std::string::npos) ? out.size() : trailer + 5;
		}
		out.replace(begin_pos, end_pos - begin_pos, "[redacted]");
	}

	// "Bearer <token>" -- redact the token, keep the word "Bearer" for context.
	{
		size_t pos = 0;
		const std::string needle = "Bearer ";
		while ((pos = out.find(needle, pos)) != std::string::npos) {
			size_t token_start = pos + needle.size();
			size_t token_end = token_start;
			while (token_end < out.size() && !std::isspace(static_cast<unsigned char>(out[token_end]))) {
				++token_end;
			}
			out.replace(token_start, token_end - token_start, "[redacted]");
			pos = token_start + std::string("[redacted]").size();
		}
	}

	// Google OAuth access-token-shaped runs, e.g. "ya29.a0ARrdaM-...".
	{
		size_t pos = 0;
		const std::string needle = "ya29.";
		while ((pos = out.find(needle, pos)) != std::string::npos) {
			size_t token_end = pos;
			while (token_end < out.size() &&
			       (std::isalnum(static_cast<unsigned char>(out[token_end])) || out[token_end] == '.' ||
			        out[token_end] == '-' || out[token_end] == '_')) {
				++token_end;
			}
			out.replace(pos, token_end - pos, "[redacted]");
			pos += std::string("[redacted]").size();
		}
	}

	return out;
}

} // namespace

bool GDriveError::IsRetryable() const {
	return kind == GDriveErrorKind::RATE_LIMIT || kind == GDriveErrorKind::TRANSIENT;
}

GDriveError ClassifyDriveError(int http_status, const std::string &body, const std::string &retry_after_header) {
	GDriveError err;
	err.http_status = http_status;
	err.retry_after_seconds = ParseRetryAfterSeconds(retry_after_header);

	ParsedBody parsed = ParseErrorBody(body);
	err.reason = parsed.reason;
	err.message = parsed.message;

	// 429 is unambiguous regardless of body: rate limit. Google's documented
	// shape puts a `reason` in the body for this case too, but a proxy/load
	// balancer can return 429 with no body or an HTML one, and the status
	// code alone is sufficient here (unlike 403, which is genuinely
	// overloaded across several unrelated conditions).
	if (http_status == 429) {
		err.kind = GDriveErrorKind::RATE_LIMIT;
		return err;
	}

	if (!parsed.reason.empty()) {
		GDriveErrorKind from_reason = ClassifyFromReason(parsed.reason);
		if (from_reason != GDriveErrorKind::UNKNOWN) {
			err.kind = from_reason;
			return err;
		}
	}

	err.kind = ClassifyFromStatus(http_status);
	return err;
}

std::string FormatUserMessage(const GDriveError &error, const std::string &context) {
	std::string out;

	switch (error.kind) {
	case GDriveErrorKind::NOT_FOUND:
		out = "no such file: " + context;
		break;
	case GDriveErrorKind::UNAUTHENTICATED:
		// REQ-NF-03: never interpolate Google's raw message here. Google's
		// 401 bodies for this path are documented to be generic ("Invalid
		// Credentials", "Login Required"), but this is exactly the error
		// class where a token/header echo is most likely to appear in some
		// future or non-standard response, so the safe choice is a fixed,
		// generic instruction rather than trusting upstream text.
		out = "authentication failed for " + context + ": the credential is missing, malformed, or expired; "
		      "re-authenticate (recreate the gdrive access configuration) and retry";
		break;
	case GDriveErrorKind::PERMISSION_DENIED:
		out = "permission denied for " + context +
		      ": the authenticated identity does not have sufficient access to this file";
		break;
	case GDriveErrorKind::STORAGE_QUOTA:
		out = "Drive storage quota exceeded while accessing " + context +
		      ": the account has no storage quota available to complete this write "
		      "(service accounts do not have storage quota outside a Shared Drive)";
		break;
	case GDriveErrorKind::RATE_LIMIT:
		out = "Drive API quota exceeded while accessing " + context + ": this request was rate-limited";
		if (error.retry_after_seconds > 0) {
			out += " and can be retried after " + std::to_string(error.retry_after_seconds) + " second(s)";
		} else {
			out += "; retry with backoff";
		}
		break;
	case GDriveErrorKind::TRANSIENT:
		out = "temporary Drive API failure while accessing " + context + ": retry with backoff";
		break;
	case GDriveErrorKind::INVALID_REQUEST:
		out = "invalid Drive API request for " + context + " (internal error, not a user mistake)";
		break;
	case GDriveErrorKind::NOT_DOWNLOADABLE:
		out = context + " has no downloadable content (a native Google format); export it instead";
		break;
	case GDriveErrorKind::UNKNOWN:
	default:
		out = "Drive API request failed for " + context;
		break;
	}

	if (!error.message.empty() && error.kind != GDriveErrorKind::UNAUTHENTICATED) {
		out += " (" + RedactCredentials(error.message) + ")";
	}

	return RedactCredentials(out);
}

GDriveExceptionType ExceptionTypeFor(GDriveErrorKind kind) {
	switch (kind) {
	case GDriveErrorKind::PERMISSION_DENIED:
		return GDriveExceptionType::PERMISSION;
	case GDriveErrorKind::INVALID_REQUEST:
		return GDriveExceptionType::INVALID_INPUT;
	case GDriveErrorKind::NOT_FOUND:
	case GDriveErrorKind::UNAUTHENTICATED:
	case GDriveErrorKind::STORAGE_QUOTA:
	case GDriveErrorKind::RATE_LIMIT:
	case GDriveErrorKind::TRANSIENT:
	case GDriveErrorKind::NOT_DOWNLOADABLE:
	case GDriveErrorKind::UNKNOWN:
	default:
		return GDriveExceptionType::IO;
	}
}

} // namespace gdrive
} // namespace duckdb
