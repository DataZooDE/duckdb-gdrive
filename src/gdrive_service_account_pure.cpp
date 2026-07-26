// S-1.5 / S-1.6 -- RFC 7523 service-account JWT assertion, pure halves.
//
// PURE: no DuckDB linkage, no network, no clock. `issued_at` is a parameter
// precisely so BuildAssertionParts is deterministic and testable against
// fixed inputs. Signing (RS256, via jwt-cpp + OpenSSL) and the token POST are
// a different translation unit (see gdrive_service_account.hpp's header
// comment for the split rationale).
#include "gdrive_service_account.hpp"

#include <cstdint>
#include <cctype>

#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

namespace duckdb {
namespace gdrive {

namespace {

// ---------------------------------------------------------------------------
// Base64url (RFC 4648 section 5 / RFC 7515): standard base64 alphabet with
// '+' -> '-', '/' -> '_', and padding stripped entirely (no trailing '=').
// ---------------------------------------------------------------------------

constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string Base64UrlEncode(const std::string &data) {
	std::string out;
	out.reserve(((data.size() + 2) / 3) * 4);

	size_t i = 0;
	const size_t n = data.size();
	while (i + 3 <= n) {
		const unsigned int chunk = (static_cast<unsigned char>(data[i]) << 16) |
		                           (static_cast<unsigned char>(data[i + 1]) << 8) |
		                           static_cast<unsigned char>(data[i + 2]);
		out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
		out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
		out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
		out.push_back(kBase64Alphabet[chunk & 0x3F]);
		i += 3;
	}

	const size_t remaining = n - i;
	if (remaining == 1) {
		const unsigned int chunk = static_cast<unsigned char>(data[i]) << 16;
		out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
		out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
		// No third/fourth character and no padding: unpadded base64url drops
		// the trailing '=' characters a length-1 tail would otherwise need.
	} else if (remaining == 2) {
		const unsigned int chunk =
		    (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
		out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
		out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
		out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
	}

	for (auto &c : out) {
		if (c == '+') {
			c = '-';
		} else if (c == '/') {
			c = '_';
		}
	}
	return out;
}

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON string escaping for the small, fixed set of fields we ever put
// into the header/claims objects. Deliberately hand-rolled rather than routed
// through picojson::value::serialize(): picojson escapes '/' as "\/", which
// is legal JSON but would make every https:// URL in the claims ugly and
// would fight the byte-exact tests for no benefit. This only ever escapes
// what RFC 8259 actually requires: '"', '\\', and control characters.
// ---------------------------------------------------------------------------
std::string JsonEscape(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 2);
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	return out;
}

std::string JsonStringField(const std::string &value) {
	return "\"" + JsonEscape(value) + "\"";
}

// Google caps the assertion lifetime at 3600 seconds (documented on
// AssertionParts / BuildAssertionParts in the header). Decision: CLAMP rather
// than reject a caller-supplied value above the cap. Google's own token
// endpoint would not honour a longer lifetime anyway, so a hard error here
// would just be a more surprising way to discover the same ceiling for what
// is otherwise a perfectly valid configuration. Non-positive lifetimes are
// still rejected outright -- there is no sensible clamp target for those.
constexpr int kMaxLifetimeSeconds = 3600;

} // namespace

AssertionParts BuildAssertionParts(const ServiceAccountKey &key, const std::string &scope, int64_t issued_at,
                                    int lifetime_seconds, const std::string &subject) {
	AssertionParts result;

	if (key.client_email.empty()) {
		result.error = "BuildAssertionParts: service-account key has no client_email";
		return result;
	}
	if (scope.empty()) {
		result.error = "BuildAssertionParts: scope must not be empty";
		return result;
	}
	if (lifetime_seconds <= 0) {
		result.error = "BuildAssertionParts: lifetime_seconds must be positive";
		return result;
	}

	const int effective_lifetime = lifetime_seconds > kMaxLifetimeSeconds ? kMaxLifetimeSeconds : lifetime_seconds;
	const std::string aud = key.token_uri.empty() ? std::string(GOOGLE_TOKEN_URL) : key.token_uri;
	const int64_t exp = issued_at + effective_lifetime;

	// Header: {"alg":"RS256","typ":"JWT","kid":"..."} -- key order matches
	// the header comment on BuildAssertionParts in gdrive_service_account.hpp.
	result.header_json =
	    "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":" + JsonStringField(key.private_key_id) + "}";

	// Claims: iss, scope, aud, exp, iat, and sub ONLY when a subject was
	// given -- Google rejects an empty "sub" claim, so omitting the key
	// entirely (rather than emitting `"sub":""`) is the only safe behaviour
	// when no subject is supplied.
	std::string claims = "{\"iss\":" + JsonStringField(key.client_email) + ",\"scope\":" + JsonStringField(scope) +
	                      ",\"aud\":" + JsonStringField(aud) + ",\"exp\":" + std::to_string(exp) +
	                      ",\"iat\":" + std::to_string(issued_at);
	if (!subject.empty()) {
		claims += ",\"sub\":" + JsonStringField(subject);
	}
	claims += "}";
	result.claims_json = claims;

	result.signing_input = Base64UrlEncode(result.header_json) + "." + Base64UrlEncode(result.claims_json);
	result.ok = true;
	return result;
}

namespace {

const std::string *FindString(const picojson::object &obj, const std::string &key) {
	auto it = obj.find(key);
	if (it == obj.end() || !it->second.is<std::string>()) {
		return nullptr;
	}
	return &it->second.get<std::string>();
}

} // namespace

ServiceAccountKeyParse ParseServiceAccountKey(const std::string &json_text) {
	ServiceAccountKeyParse result;

	picojson::value root;
	// REQ-NF-03: on a JSON syntax error, picojson's own error string can
	// quote a fragment of the input near the parse failure -- which could be
	// the private key material if that's where the document broke. Never
	// forward picojson's parse-error text; use a fixed, generic message.
	std::string parse_err = picojson::parse(root, json_text);
	if (!parse_err.empty()) {
		result.error = "service-account key document is not valid JSON";
		return result;
	}
	if (!root.is<picojson::object>()) {
		result.error = "service-account key document must be a JSON object";
		return result;
	}
	const picojson::object &obj = root.get<picojson::object>();

	// A common mistake: handing this an OAuth *client* JSON (the "Download
	// JSON" button for an OAuth client ID in Cloud Console) instead of a
	// service-account key. That document has no top-level "type" at all --
	// it's wrapped in "installed" or "web" -- so this check has to happen
	// before (and independently of) the "type" check below.
	if (obj.find("installed") != obj.end() || obj.find("web") != obj.end()) {
		result.error = "this looks like an OAuth *client* JSON (has an \"installed\"/\"web\" section), not a "
		               "service-account key; download the service-account key from IAM & Admin > Service "
		               "Accounts > Keys instead";
		return result;
	}

	if (const std::string *type = FindString(obj, "type")) {
		if (*type != "service_account") {
			result.error = "service-account key has \"type\":\"" + JsonEscape(*type) +
			               "\" but expected \"service_account\"";
			return result;
		}
	}

	const std::string *client_email = FindString(obj, "client_email");
	if (!client_email || client_email->empty()) {
		result.error = "service-account key is missing required field \"client_email\"";
		return result;
	}

	const std::string *private_key = FindString(obj, "private_key");
	if (!private_key || private_key->empty()) {
		result.error = "service-account key is missing required field \"private_key\"";
		return result;
	}

	result.key.client_email = *client_email;
	result.key.private_key = *private_key;
	if (const std::string *kid = FindString(obj, "private_key_id")) {
		result.key.private_key_id = *kid;
	}
	if (const std::string *project = FindString(obj, "project_id")) {
		result.key.project_id = *project;
	}
	if (const std::string *token_uri = FindString(obj, "token_uri")) {
		if (!token_uri->empty()) {
			result.key.token_uri = *token_uri;
		}
	}
	if (result.key.token_uri.empty()) {
		result.key.token_uri = GOOGLE_TOKEN_URL;
	}

	result.ok = true;
	return result;
}

} // namespace gdrive
} // namespace duckdb
