// C-2 -- the Google-specific half of the authorization-code flow.
//
// Pure configuration construction, so it is Catch2's. The flow that consumes
// these values lives in datazoo-oauth2 and has its own tests; what is asserted
// here is only the part that knows Google exists.
#include "gdrive_oauth_params.hpp"
#include "gdrive_service_account.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace duckdb::gdrive;

TEST_CASE("Google requires access_type=offline and prompt=consent for a refresh token", "[oauth]") {
	// Without BOTH, Google returns an access token and no refresh token. The
	// flow appears to succeed and then stops working an hour later, which is
	// a considerably worse failure than an outright error.
	auto params = BuildGoogleOAuthParams("");

	REQUIRE(params.extra_auth_params.at("access_type") == "offline");
	REQUIRE(params.extra_auth_params.at("prompt") == "consent");
}

TEST_CASE("the endpoints are always set explicitly, never left to defaults", "[oauth]") {
	// OAuth2Config::GetAuthorizationUrl()/GetTokenUrl() default to SAP BTP's
	// URL shape when no custom URL is set (datazoo-oauth2
	// docs/EXTRACTION_NOTES.md). Leaving either empty would silently send a
	// Google consent request to a SAP-shaped hostname.
	auto params = BuildGoogleOAuthParams("");

	REQUIRE(params.auth_url == "https://accounts.google.com/o/oauth2/v2/auth");
	REQUIRE(params.token_url == "https://oauth2.googleapis.com/token");
	REQUIRE_FALSE(params.auth_url.empty());
	REQUIRE_FALSE(params.token_url.empty());
	REQUIRE(params.auth_url.find("hana.ondemand.com") == std::string::npos);
}

TEST_CASE("an unspecified scope requests the narrowest one that works", "[oauth]") {
	// REQ-NF-04.
	auto params = BuildGoogleOAuthParams("");
	REQUIRE(params.scope == std::string(SCOPE_DRIVE_READONLY));
}

TEST_CASE("an explicit scope is passed through unchanged", "[oauth]") {
	auto params = BuildGoogleOAuthParams(SCOPE_DRIVE);
	REQUIRE(params.scope == std::string(SCOPE_DRIVE));
}

// ---------------------------------------------------------------------------
// C-5 -- headless hosts must fail fast, not hang.
//
// datazoo-oauth2's OAuth2Browser shells out to xdg-open without checking
// whether a display exists. On a headless box that blocks until the callback
// timeout, so the user waits minutes to be told "timed out waiting for
// authorization" -- which sounds like they were too slow, not like the
// machine never had a browser.
// ---------------------------------------------------------------------------

TEST_CASE("a Linux host with no display cannot launch a browser", "[oauth]") {
	DisplayEnvironment env;
	env.is_posix_non_apple = true;
	REQUIRE_FALSE(CanLaunchBrowser(env));
}

TEST_CASE("an X11 display is enough", "[oauth]") {
	DisplayEnvironment env;
	env.is_posix_non_apple = true;
	env.display = ":0";
	REQUIRE(CanLaunchBrowser(env));
}

TEST_CASE("a Wayland display is enough", "[oauth]") {
	DisplayEnvironment env;
	env.is_posix_non_apple = true;
	env.wayland_display = "wayland-0";
	REQUIRE(CanLaunchBrowser(env));
}

TEST_CASE("SSH with X11 forwarding still counts as having a browser", "[oauth]") {
	// SSH_CONNECTION alone must NOT veto: forwarded X11 sets DISPLAY and the
	// browser genuinely opens, on the user's own machine. Treating any SSH
	// session as headless would break a working setup.
	DisplayEnvironment env;
	env.is_posix_non_apple = true;
	env.display = "localhost:10.0";
	env.ssh_connection = "10.0.0.1 51234 10.0.0.2 22";
	REQUIRE(CanLaunchBrowser(env));
}

TEST_CASE("macOS and Windows are always assumed to have a browser", "[oauth]") {
	// `open` and ShellExecute work on any desktop session and there is no
	// cheap, reliable signal for their absence. Guessing would be worse than
	// letting the flow report its own failure.
	DisplayEnvironment env;
	env.is_posix_non_apple = false;
	REQUIRE(CanLaunchBrowser(env));
}

TEST_CASE("the no-browser message points at the providers that need no browser", "[oauth]") {
	const std::string msg = NoBrowserMessage();
	REQUIRE(msg.find("credential_chain") != std::string::npos);
	REQUIRE(msg.find("service_account") != std::string::npos);
}
