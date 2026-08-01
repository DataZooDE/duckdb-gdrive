// C-2 -- the Google-specific half of the authorization-code flow, pure.
//
// PURE: no DuckDB linkage, no network, no I/O. See gdrive_oauth_params.hpp
// for the two traps this file exists to encode.
#include "gdrive_oauth_params.hpp"
#include "gdrive_service_account.hpp"

namespace duckdb {
namespace gdrive {

GoogleOAuthParams BuildGoogleOAuthParams(const std::string &requested_scope) {
	GoogleOAuthParams params;

	// Always explicit -- never rely on OAuth2Config's defaults, which are SAP
	// BTP-shaped. These two constants are shared with the service-account
	// path, so there is exactly one place where Google's endpoints are named.
	params.auth_url = GOOGLE_AUTH_URL;
	params.token_url = GOOGLE_TOKEN_URL;

	params.scope = requested_scope.empty() ? std::string(SCOPE_DRIVE_READONLY) : requested_scope;

	// Google issues no refresh token unless BOTH are present.
	//
	// access_type=offline asks for one at all. prompt=consent forces the
	// consent screen even when the user has already approved this client --
	// without it, a second authorization returns an access token and NO
	// refresh token, because Google only mints a refresh token on the grant
	// that actually shows the consent screen. That makes the bug appear only
	// on re-authorization, which is the worst possible time to discover it.
	params.extra_auth_params["access_type"] = "offline";
	params.extra_auth_params["prompt"] = "consent";

	return params;
}


bool CanLaunchBrowser(const DisplayEnvironment &env) {
	if (!env.is_posix_non_apple) {
		// macOS/Windows: `open` and ShellExecute work on any desktop session,
		// and there is no cheap, reliable signal for their absence. Guessing
		// wrong here would block a flow that would have worked, so assume yes
		// and let the flow report its own failure.
		return true;
	}

	// X11 or Wayland. Either is sufficient; both absent means no display
	// server, and xdg-open has nothing to talk to.
	//
	// SSH_CONNECTION is deliberately NOT a veto. With X11 forwarding, DISPLAY
	// is set and the browser really does open -- on the user's own machine,
	// which is exactly where consent should happen. Treating every SSH session
	// as headless would break a setup that works.
	return !env.display.empty() || !env.wayland_display.empty();
}

std::string NoBrowserMessage() {
	return "no browser is available on this machine (neither DISPLAY nor WAYLAND_DISPLAY is set), "
	       "so the interactive consent flow cannot run.\n"
	       "Use a provider that needs no browser:\n"
	       "  CREATE SECRET (TYPE gdrive, PROVIDER credential_chain);\n"
	       "    -- uses `gcloud auth application-default login`, run on a machine that has one\n"
	       "  CREATE SECRET (TYPE gdrive, PROVIDER service_account, KEY_FILE '/path/to/key.json');\n"
	       "Or complete the consent flow on a desktop machine and copy the resulting refresh token "
	       "into a PROVIDER config secret.";
}

} // namespace gdrive
} // namespace duckdb
