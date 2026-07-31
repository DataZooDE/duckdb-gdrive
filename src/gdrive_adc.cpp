// B-1 / B-2 -- Application Default Credentials discovery, pure halves.
//
// PURE: no DuckDB linkage, no network, no filesystem, no getenv(). The
// environment arrives as an AdcPathInputs struct and the file contents arrive
// as a string, precisely so the precedence order and the document dispatch are
// testable against fixed inputs. See gdrive_adc.hpp for why we resolve ADC
// ourselves instead of linking google-cloud-cpp (short version: its token
// generator hands back a self-signed JWT for service accounts, which Drive
// rejects with 401).
#include "gdrive_adc.hpp"

#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

namespace duckdb {
namespace gdrive {

namespace {

constexpr const char *kAdcFileName = "application_default_credentials.json";

//! Join without collapsing separators. Windows accepts '/' in API paths, and
//! using it unconditionally keeps this function free of platform branching --
//! which matters because it is pure and must behave identically wherever the
//! Catch2 binary happens to be built.
std::string Join(const std::string &dir, const std::string &leaf) {
	if (dir.empty()) {
		return leaf;
	}
	const char last = dir[dir.size() - 1];
	if (last == '/' || last == '\\') {
		return dir + leaf;
	}
	return dir + "/" + leaf;
}

std::string FindString(const picojson::object &obj, const char *key) {
	auto it = obj.find(key);
	if (it != obj.end() && it->second.is<std::string>()) {
		return it->second.get<std::string>();
	}
	return "";
}

} // namespace

std::string ResolveAdcPath(const AdcPathInputs &inputs) {
	// 1. An explicit key file always wins. This is the variable Google's own
	//    libraries honour first, and the one CI sets.
	if (!inputs.google_application_credentials.empty()) {
		return inputs.google_application_credentials;
	}

	// 2. CLOUDSDK_CONFIG relocates gcloud's whole config directory. Someone who
	//    has set it has done so deliberately; honouring it is what makes a
	//    per-project or containerised gcloud config work.
	if (!inputs.cloudsdk_config.empty()) {
		return Join(inputs.cloudsdk_config, kAdcFileName);
	}

	// 3. The well-known location. HOME is checked before APPDATA so that a
	//    POSIX-shaped environment on Windows (MSYS, Git Bash, WSL interop)
	//    resolves the way the user's shell would.
	if (!inputs.home.empty()) {
		return Join(Join(inputs.home, ".config/gcloud"), kAdcFileName);
	}
	if (!inputs.appdata.empty()) {
		return Join(Join(inputs.appdata, "gcloud"), kAdcFileName);
	}

	// Nothing resolvable. Returning "" rather than a half-built path keeps the
	// caller from reporting a file-not-found for a path the user never had.
	return "";
}

AdcParse ParseAdcJson(const std::string &json_text) {
	AdcParse result;

	picojson::value root;
	const std::string parse_error = picojson::parse(root, json_text);
	if (!parse_error.empty() || !root.is<picojson::object>()) {
		// REQ-NF-03: picojson's own error text can quote the offending input,
		// so it is deliberately NOT forwarded -- an ADC file is credential
		// material end to end.
		result.error = "not valid JSON. An Application Default Credentials file is written by "
		               "`gcloud auth application-default login`; if you edited it by hand, "
		               "re-run that command.";
		return result;
	}
	const auto &obj = root.get<picojson::object>();

	const std::string type = FindString(obj, "type");
	if (type.empty()) {
		// An OAuth *client* JSON downloaded from the Cloud Console has no
		// `type` and wraps everything in "installed" or "web". Naming that
		// specific mistake is far more useful than "missing type".
		if (obj.find("installed") != obj.end() || obj.find("web") != obj.end()) {
			result.error =
			    "this looks like an OAuth client JSON (it has an \"installed\" or \"web\" "
			    "wrapper and no \"type\" field), not an Application Default Credentials file. "
			    "An OAuth client is used with PROVIDER authorization_code, not credential_chain.";
			return result;
		}
		result.error = "no \"type\" field, so this is not an Application Default Credentials "
		               "document. Expected \"authorized_user\" or \"service_account\".";
		return result;
	}

	if (type == "authorized_user") {
		result.kind = AdcKind::AUTHORIZED_USER;
		result.user.client_id = FindString(obj, "client_id");
		result.user.client_secret = FindString(obj, "client_secret");
		result.user.refresh_token = FindString(obj, "refresh_token");
		result.user.quota_project_id = FindString(obj, "quota_project_id");

		// All three are needed to mint an access token; report which one is
		// missing by NAME only, never by value.
		if (result.user.client_id.empty() || result.user.client_secret.empty() ||
		    result.user.refresh_token.empty()) {
			std::string missing;
			auto note = [&missing](const char *field) {
				if (!missing.empty()) {
					missing += ", ";
				}
				missing += field;
			};
			if (result.user.client_id.empty()) {
				note("client_id");
			}
			if (result.user.client_secret.empty()) {
				note("client_secret");
			}
			if (result.user.refresh_token.empty()) {
				note("refresh_token");
			}
			result.error = "an \"authorized_user\" document is missing " + missing +
			                ". Re-run `gcloud auth application-default login`.";
			return result;
		}

		result.ok = true;
		return result;
	}

	if (type == "service_account") {
		result.kind = AdcKind::SERVICE_ACCOUNT;
		// Delegated so the two entry points cannot disagree about what a valid
		// key looks like, and so the "never quote the private key" guarantee
		// lives in exactly one place.
		ServiceAccountKeyParse key = ParseServiceAccountKey(json_text);
		if (!key.ok) {
			result.error = key.error;
			return result;
		}
		result.service_account = key.key;
		result.ok = true;
		return result;
	}

	if (type == "external_account") {
		// Workload identity federation. Recognised on purpose: falling through
		// to "unexpected type" would be true but useless, and the naive
		// alternative ("missing client_id") would be actively misleading.
		result.kind = AdcKind::EXTERNAL_ACCOUNT;
		result.error =
		    "\"external_account\" credentials (workload identity federation) are not supported "
		    "by the gdrive extension. Use PROVIDER service_account with a key file, or "
		    "`gcloud auth application-default login` for a user credential.";
		return result;
	}

	// The type value itself is a short, fixed enum-like string, never secret.
	result.error = "unexpected credential type \"" + type +
	                "\". Expected \"authorized_user\" or \"service_account\".";
	return result;
}

std::string NoCredentialsMessage() {
	// Written as a map out, not a report of failure. Ordered by how likely the
	// reader is to want it: the interactive case first.
	return "gdrive: no Google credentials found.\n"
	       "\n"
	       "1. Log in with your own Google account (most common):\n"
	       "     gcloud auth application-default login \\\n"
	       "       --scopes=openid,https://www.googleapis.com/auth/drive\n"
	       "\n"
	       "   The --scopes flag is required. Without it gcloud requests\n"
	       "   cloud-platform, which does NOT include Drive, and every read\n"
	       "   fails with a 403 that looks like a permissions problem.\n"
	       "\n"
	       "   Note: plain `gcloud auth login` is not sufficient -- it\n"
	       "   configures the CLI, not Application Default Credentials.\n"
	       "\n"
	       "2. Point at a service-account key:\n"
	       "     export GOOGLE_APPLICATION_CREDENTIALS=/path/to/key.json\n"
	       "\n"
	       "3. Or configure a secret explicitly:\n"
	       "     CREATE SECRET (TYPE gdrive, PROVIDER service_account,\n"
	       "                    KEY_FILE '/path/to/key.json');\n"
	       "     CREATE SECRET (TYPE gdrive, PROVIDER config, ACCESS_TOKEN '...');\n";
}

} // namespace gdrive
} // namespace duckdb
