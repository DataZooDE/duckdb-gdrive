#pragma once

#include <map>
#include <string>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// The Google-specific half of the authorization-code flow (C-2).
//
// PURE: builds the configuration values and nothing else. The flow itself --
// PKCE, the loopback server, the browser launch, the code exchange -- is
// datazoo-oauth2's, and is provider-agnostic by design (REQ-A-03). This is
// the file that knows Google exists.
//
// Two traps are encoded here rather than left to a call site to remember,
// both documented in datazoo-oauth2's docs/EXTRACTION_NOTES.md:
//
//   1. The endpoint URLs must ALWAYS be set explicitly. OAuth2Config's
//      GetAuthorizationUrl()/GetTokenUrl() fall back to SAP BTP's URL format
//      when no custom URL is set -- genuine vendor logic left in a shared
//      type because removing it would have changed behaviour erpl-web's tests
//      depend on. It never fires for us *provided* we always set our own.
//
//   2. access_type=offline and prompt=consent are not optional. Google issues
//      NO refresh token without both, and a flow that silently yields an
//      access token good for one hour looks like it worked. That is precisely
//      the failure mode the gsheets extension ships with.
// ---------------------------------------------------------------------------

//! Google's OAuth2 endpoints and the extra authorization parameters Google
//! requires, ready to be copied onto an OAuth2Config.
struct GoogleOAuthParams {
	std::string auth_url;
	std::string token_url;
	std::string scope;
	//! Appended to the authorization URL as additional query parameters.
	//! Empty for erpl-web; non-empty here, which is the whole reason
	//! extra_auth_params was added to the shared config during extraction.
	std::map<std::string, std::string> extra_auth_params;
};

//! Build the Google parameters for a requested Drive scope.
//!
//! `requested_scope` empty means the narrowest scope that works (REQ-NF-04).
GoogleOAuthParams BuildGoogleOAuthParams(const std::string &requested_scope);

//! Whether a browser can plausibly be launched, given the display environment.
//!
//! datazoo-oauth2's OAuth2Browser does no such check: on Linux it shells out
//! to xdg-open and hopes. On a headless host that either fails obscurely or
//! blocks until the callback handler's timeout, and "my query hung for two
//! minutes and then said 'timed out waiting for authorization'" is a bad way
//! to learn that the machine has no browser.
//!
//! The check is deliberately here rather than in the shared library, whose
//! behaviour erpl-web depends on.
//!
//! Parameters rather than getenv() so this stays pure and testable. On macOS
//! and Windows a browser is always assumed available -- `open` and
//! ShellExecute work on any desktop session, and there is no equivalent
//! cheap, reliable signal for their absence.
struct DisplayEnvironment {
	bool is_posix_non_apple = false; //!< Linux/BSD: DISPLAY/WAYLAND_DISPLAY apply.
	std::string display;             //!< $DISPLAY
	std::string wayland_display;     //!< $WAYLAND_DISPLAY
	std::string ssh_connection;      //!< $SSH_CONNECTION -- advisory only.
};

bool CanLaunchBrowser(const DisplayEnvironment &env);

//! The message shown when no browser can be launched. Names the two providers
//! that work without one, because that is the actual next step.
std::string NoBrowserMessage();

} // namespace gdrive
} // namespace duckdb
