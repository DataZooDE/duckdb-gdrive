// S-2.3 -- Drive error classification. PURE: no duckdb.hpp, no I/O.
//
// See src/include/gdrive_errors.hpp for the contract and rationale.
#include "gdrive_errors.hpp"

#include <cstdint>
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
	//! `error.details[].reason` -- the google.rpc.ErrorInfo reason, which is a
	//! DIFFERENT and more specific vocabulary than errors[0].reason. Notably
	//! it is the only thing separating "your token lacks the Drive scope"
	//! (ACCESS_TOKEN_SCOPE_INSUFFICIENT) from "you cannot read this file":
	//! both carry errors[0].reason "insufficientPermissions".
	std::string detail_reason;
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

	// error.details[] is an array of typed google.rpc payloads; we want the
	// ErrorInfo one's `reason`. Scanning every element rather than assuming
	// [0] because the array also carries Help/LocalizedMessage entries whose
	// order Google does not document.
	auto details_it = error_obj.find("details");
	if (details_it != error_obj.end() && details_it->second.is<picojson::array>()) {
		for (const auto &detail : details_it->second.get<picojson::array>()) {
			if (!detail.is<picojson::object>()) {
				continue;
			}
			const auto &detail_obj = detail.get<picojson::object>();
			auto reason_it = detail_obj.find("reason");
			if (reason_it != detail_obj.end() && reason_it->second.is<std::string>()) {
				out.detail_reason = reason_it->second.get<std::string>();
				break;
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
// This is DEFENCE IN DEPTH, not the primary control. The primary control is
// that credentials never enter the code paths that construct these strings
// in the first place (they come only from Drive's own response body, which
// should not contain them). This function exists because "should not" is not
// "cannot": Drive is a third party, proxies/relays can echo back request
// headers in diagnostic bodies, and classification runs on untrusted bytes.
//
// Heuristics, deliberately simple and over-inclusive (a false positive here
// just means a slightly more generic message, which is the safe direction):
//   - "Bearer <token>", case-insensitive, any run of horizontal whitespace
//     (space/tab, one or more) between the keyword and the token
//                                          -> "Bearer [redacted]"
//   - "Authorization: <scheme> <value>"    -> "Authorization: <scheme>
//     (any scheme, e.g. "Basic")              [redacted]"
//   - an OAuth access-token-shaped run      -> "[redacted]"
//     starting "ya29." (Google's own prefix for user access tokens)
//   - a Google refresh-token-shaped run     -> "[redacted]"
//     starting "1//" (Google's own prefix for refresh tokens)
//   - the value half of a JSON-ish or       -> "[redacted]"
//     query-string key/value pair whose key
//     is one of: access_token, refresh_token, private_key, client_secret,
//     id_token. Handles `"key": "value"`, `key=value`, and `key value`.
//     Deliberately excludes client_id -- it identifies the OAuth client, not
//     a secret, and redacting it would make error messages strictly less
//     useful for no security benefit.
//   - a PEM block (BEGIN/END ... KEY)       -> "[redacted]"
//
// NOT attempted: generic detection of "base64-shaped blobs" as a blanket
// rule. Nearly anything can look base64-shaped (file ids, hashes, ordinary
// words), so a context-free rule would over-redact and silently degrade
// unrelated diagnostics; the key- and scheme-anchored rules above catch the
// realistic cases (JSON bodies, query strings, Authorization headers)
// without that cost.
// ---------------------------------------------------------------------------

//! Case-insensitive substring search, ASCII-only (sufficient here: every
//! needle used below is an ASCII keyword).
size_t FindCaseInsensitive(const std::string &haystack, const std::string &needle_lower, size_t from) {
	if (needle_lower.size() > haystack.size() || from > haystack.size() - needle_lower.size()) {
		return std::string::npos;
	}
	for (size_t i = from; i + needle_lower.size() <= haystack.size(); ++i) {
		bool match = true;
		for (size_t j = 0; j < needle_lower.size(); ++j) {
			if (std::tolower(static_cast<unsigned char>(haystack[i + j])) != needle_lower[j]) {
				match = false;
				break;
			}
		}
		if (match) {
			return i;
		}
	}
	return std::string::npos;
}

bool IsIdentifierChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

const std::string kRedacted = "[redacted]";

//! Redact the token following a case-insensitive `keyword` (e.g. "bearer"),
//! separated from it by one or more horizontal whitespace characters. If no
//! whitespace immediately follows a match (e.g. the keyword is part of a
//! longer word), that occurrence is left untouched and the search continues
//! past it -- this is what keeps "bearerish" from being mangled.
void RedactKeywordToken(std::string &out, const std::string &keyword_lower) {
	size_t pos = 0;
	while ((pos = FindCaseInsensitive(out, keyword_lower, pos)) != std::string::npos) {
		size_t after_keyword = pos + keyword_lower.size();
		size_t token_start = after_keyword;
		while (token_start < out.size() && (out[token_start] == ' ' || out[token_start] == '\t')) {
			++token_start;
		}
		if (token_start == after_keyword) {
			pos = after_keyword;
			continue;
		}
		size_t token_end = token_start;
		while (token_end < out.size() && !std::isspace(static_cast<unsigned char>(out[token_end]))) {
			++token_end;
		}
		if (token_end == token_start) {
			pos = after_keyword;
			continue;
		}
		out.replace(token_start, token_end - token_start, kRedacted);
		pos = token_start + kRedacted.size();
	}
}

//! Redact the run of token-shaped characters (alnum, '.', '-', '_', '/')
//! immediately following a literal `prefix` (e.g. "ya29." or "1//").
void RedactPrefixedRun(std::string &out, const std::string &prefix) {
	size_t pos = 0;
	while ((pos = out.find(prefix, pos)) != std::string::npos) {
		size_t token_end = pos;
		while (token_end < out.size() &&
		       (std::isalnum(static_cast<unsigned char>(out[token_end])) || out[token_end] == '.' ||
		        out[token_end] == '-' || out[token_end] == '_' || out[token_end] == '/')) {
			++token_end;
		}
		out.replace(pos, token_end - pos, kRedacted);
		pos += kRedacted.size();
	}
}

//! Redact the value half of a `key`/value pair wherever `key` appears as a
//! whole word (not as part of a longer identifier), covering the JSON
//! (`"key": "value"`), query-string (`key=value`) and plain-text
//! (`key value`) spellings uniformly. If `key` is not followed by a
//! recognizable key/value delimiter, the occurrence is left alone -- this is
//! what keeps a key name mentioned in prose from mangling unrelated text.
void RedactKeyValue(std::string &out, const std::string &key_lower) {
	size_t pos = 0;
	while ((pos = FindCaseInsensitive(out, key_lower, pos)) != std::string::npos) {
		size_t match_end = pos + key_lower.size();
		// Word-boundary check: a real key is not preceded or followed by
		// another identifier character (rules out "my_client_secretary").
		bool boundary_before = (pos == 0) || !IsIdentifierChar(out[pos - 1]);
		bool boundary_after = (match_end == out.size()) || !IsIdentifierChar(out[match_end]);
		if (!boundary_before || !boundary_after) {
			pos = match_end;
			continue;
		}

		size_t i = match_end;
		// Skip the key's own closing quote (if any), then ':' or '=', then
		// whitespace, then the value's opening quote (if any).
		while (i < out.size() && (out[i] == '"' || out[i] == '\'' || out[i] == ':' || out[i] == '=' ||
		                          out[i] == ' ' || out[i] == '\t')) {
			++i;
		}
		if (i == match_end) {
			// No delimiter followed the key at all -- not a key/value
			// occurrence (e.g. the bare word appears in prose).
			pos = match_end;
			continue;
		}

		size_t value_start = i;
		size_t value_end = value_start;
		while (value_end < out.size() && out[value_end] != '"' && out[value_end] != '\'' &&
		       out[value_end] != ',' && out[value_end] != '}' && out[value_end] != '&' &&
		       !std::isspace(static_cast<unsigned char>(out[value_end]))) {
			++value_end;
		}
		if (value_end == value_start) {
			pos = match_end;
			continue;
		}
		out.replace(value_start, value_end - value_start, kRedacted);
		pos = value_start + kRedacted.size();
	}
}

//! Redact "Authorization: <scheme> <value>" wholesale (both HTTP-header and
//! prose spellings), leaving only the scheme name (e.g. "Basic") for
//! context. This is in addition to the standalone Bearer handling above
//! because a non-Bearer scheme (Basic, Digest, ...) never contains the word
//! "Bearer" for that rule to anchor on.
void RedactAuthorizationHeader(std::string &out) {
	size_t pos = 0;
	const std::string needle_lower = "authorization";
	while ((pos = FindCaseInsensitive(out, needle_lower, pos)) != std::string::npos) {
		size_t i = pos + needle_lower.size();
		while (i < out.size() && (out[i] == ':' || out[i] == ' ' || out[i] == '\t')) {
			++i;
		}
		// Skip the scheme word (Bearer / Basic / Digest / ...), if present.
		size_t scheme_end = i;
		while (scheme_end < out.size() && !std::isspace(static_cast<unsigned char>(out[scheme_end]))) {
			++scheme_end;
		}
		if (scheme_end == i) {
			pos = i;
			continue;
		}
		size_t value_start = scheme_end;
		while (value_start < out.size() &&
		       (out[value_start] == ' ' || out[value_start] == '\t')) {
			++value_start;
		}
		if (value_start == scheme_end) {
			pos = scheme_end;
			continue;
		}
		size_t value_end = value_start;
		while (value_end < out.size() && !std::isspace(static_cast<unsigned char>(out[value_end]))) {
			++value_end;
		}
		if (value_end == value_start) {
			pos = scheme_end;
			continue;
		}
		out.replace(value_start, value_end - value_start, kRedacted);
		pos = value_start + kRedacted.size();
	}
}

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
		out.replace(begin_pos, end_pos - begin_pos, kRedacted);
	}

	RedactAuthorizationHeader(out);
	RedactKeywordToken(out, "bearer");
	RedactPrefixedRun(out, "ya29.");
	RedactPrefixedRun(out, "1//");

	for (const char *key : {"access_token", "refresh_token", "private_key", "client_secret", "id_token"}) {
		RedactKeyValue(out, key);
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

	// Checked BEFORE errors[0].reason, because that reason
	// ("insufficientPermissions") is shared with a genuine file-sharing
	// denial and would otherwise win. The details[] reason is the only
	// signal that separates them. See GDriveErrorKind::INSUFFICIENT_SCOPE.
	if (parsed.detail_reason == "ACCESS_TOKEN_SCOPE_INSUFFICIENT") {
		err.kind = GDriveErrorKind::INSUFFICIENT_SCOPE;
		return err;
	}

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
	case GDriveErrorKind::INSUFFICIENT_SCOPE:
		// The file is very likely fine; the token is not. Say so, and give the
		// exact command -- this is the default outcome of `gcloud auth
		// application-default login` without --scopes, so the reader has done
		// nothing wrong and needs a fix, not a diagnosis.
		out = "insufficient OAuth scope for " + context +
		      ": the access token was not granted a Google Drive scope. This is not a "
		      "file-sharing problem.\n"
		      "If the credential came from gcloud, request the Drive scope explicitly:\n"
		      "  gcloud auth application-default login \\\n"
		      "    --scopes=openid,https://www.googleapis.com/auth/drive\n"
		      "(gcloud's default scope, cloud-platform, does not include Drive.)\n"
		      "For a gdrive secret, set DRIVE_SCOPE to a Drive scope such as "
		      "'https://www.googleapis.com/auth/drive'.";
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
	case GDriveErrorKind::INSUFFICIENT_SCOPE:
		// Both are PermissionException: DuckDB's exception taxonomy has no
		// "your credential is under-scoped" type, and Permission is the
		// closest honest fit. The distinction that matters is carried by the
		// message, which is where the reader looks.
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
