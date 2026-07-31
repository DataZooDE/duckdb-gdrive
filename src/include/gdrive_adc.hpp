#pragma once

#include "gdrive_service_account.hpp"

#include <string>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Application Default Credentials (ADC) discovery -- the `credential_chain`
// provider (decision D-10).
//
// The principle is borrowed from northpolesec/duckdb-gcs: a user who has
// already run `gcloud auth application-default login` should not have to do
// any Cloud Console work to read their own Drive. duckdb-gcs gets this by
// delegating to `google-cloud-cpp`'s credential chain; we resolve ADC
// ourselves, and the B-0 spike is why:
//
//   google::cloud::oauth2::MakeAccessTokenGenerator, given service-account
//   credentials, returns a SELF-SIGNED JWT (RS256, three segments), not an
//   OAuth2 access token. Google Cloud APIs accept those. Drive -- a Workspace
//   API -- does not: it answers 401 "Invalid Credentials". `ScopesOption` does
//   not help; it is documented as configuring
//   MakeImpersonateServiceAccountCredentials() only. Verified against the real
//   API 2026-07-31 with a real service-account key.
//
// So the SDK could only ever have served one of the chain's arms, at a cost of
// ~9 MB stripped plus a libcurl dependency. Both arms we care about map onto
// token paths this extension ALREADY has: an `authorized_user` document is
// exactly the CLIENT_ID/CLIENT_SECRET/REFRESH_TOKEN triple that PROVIDER
// config refreshes, and a `service_account` document is exactly what the
// RFC 7523 minting path consumes.
//
// Everything in this header is PURE -- environment values are parameters, not
// getenv() calls, and nothing here opens a file. The DuckDB-side caller reads
// the environment, reads the file, and turns a failure into the right
// exception type.
// ---------------------------------------------------------------------------

//! Which kind of document an ADC JSON file turned out to be.
enum class AdcKind {
	UNKNOWN,
	AUTHORIZED_USER, //!< `gcloud auth application-default login`
	SERVICE_ACCOUNT, //!< a downloaded service-account key
	EXTERNAL_ACCOUNT //!< workload identity federation -- recognised, unsupported
};

//! The refreshable triple out of an `authorized_user` ADC document. These are
//! the same three fields PROVIDER config needs, deliberately.
struct AdcUserCredentials {
	std::string client_id;
	std::string client_secret; //!< NEVER log, never put in an error message.
	std::string refresh_token; //!< NEVER log, never put in an error message.
	std::string quota_project_id;
};

struct AdcParse {
	bool ok = false;
	std::string error; //!< Must never quote client_secret or refresh_token.
	AdcKind kind = AdcKind::UNKNOWN;
	AdcUserCredentials user;           //!< Valid when kind == AUTHORIZED_USER.
	ServiceAccountKey service_account; //!< Valid when kind == SERVICE_ACCOUNT.
};

//! Environment inputs to ADC path resolution. Passed in rather than read, so
//! the resolution order is testable without mutating the process environment.
struct AdcPathInputs {
	std::string google_application_credentials; //!< $GOOGLE_APPLICATION_CREDENTIALS
	std::string cloudsdk_config;                //!< $CLOUDSDK_CONFIG
	std::string home;                           //!< $HOME (POSIX)
	std::string appdata;                        //!< %APPDATA% (Windows)
};

//! Resolve the ADC file path, in Google's documented precedence order:
//!
//!   1. $GOOGLE_APPLICATION_CREDENTIALS, verbatim, if set
//!   2. $CLOUDSDK_CONFIG/application_default_credentials.json, if set
//!   3. %APPDATA%/gcloud/application_default_credentials.json  (Windows)
//!      $HOME/.config/gcloud/application_default_credentials.json (otherwise)
//!
//! Returns "" when nothing can be resolved (no env vars and no home).
std::string ResolveAdcPath(const AdcPathInputs &inputs);

//! Parse an ADC JSON document, dispatching on its `type` field.
//!
//! A `service_account` document is delegated to ParseServiceAccountKey, so the
//! two paths cannot disagree about what a valid key looks like.
//!
//! REQ-NF-03: no error message produced here may contain any part of
//! `client_secret` or `refresh_token`, and there is a test asserting it.
AdcParse ParseAdcJson(const std::string &json_text);

//! The message shown when no credential could be resolved.
//!
//! Deliberately teaches the fix rather than reporting a failure. In
//! particular it must call out that plain `gcloud auth login` is NOT
//! sufficient -- that is the single most common mistake -- and that the Drive
//! scope has to be requested explicitly, because gcloud's ADC default scope
//! (cloud-platform) does not include Drive and the resulting failure is a 403
//! that reads like a permissions problem.
std::string NoCredentialsMessage();

} // namespace gdrive
} // namespace duckdb
