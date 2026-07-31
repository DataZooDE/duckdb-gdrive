// B-1 / B-2 -- ADC discovery, pure halves.
//
// The `credential_chain` provider's file-location logic and JSON dispatch are
// pure, so they are Catch2's. The live SQL suite covers the other half (that
// a resolved credential actually reads from Drive); per the repo's rule, no
// behaviour is covered by both layers.
#include "gdrive_adc.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace duckdb::gdrive;

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

TEST_CASE("GOOGLE_APPLICATION_CREDENTIALS wins over everything else", "[adc]") {
	AdcPathInputs in;
	in.google_application_credentials = "/explicit/key.json";
	in.cloudsdk_config = "/cfg";
	in.home = "/home/u";
	REQUIRE(ResolveAdcPath(in) == "/explicit/key.json");
}

TEST_CASE("CLOUDSDK_CONFIG overrides the default gcloud config directory", "[adc]") {
	AdcPathInputs in;
	in.cloudsdk_config = "/cfg";
	in.home = "/home/u";
	REQUIRE(ResolveAdcPath(in) == "/cfg/application_default_credentials.json");
}

TEST_CASE("default POSIX location is under ~/.config/gcloud", "[adc]") {
	AdcPathInputs in;
	in.home = "/home/u";
	REQUIRE(ResolveAdcPath(in) == "/home/u/.config/gcloud/application_default_credentials.json");
}

TEST_CASE("APPDATA is used when there is no HOME", "[adc]") {
	AdcPathInputs in;
	in.appdata = "C:\\Users\\u\\AppData\\Roaming";
	REQUIRE(ResolveAdcPath(in) ==
	        "C:\\Users\\u\\AppData\\Roaming/gcloud/application_default_credentials.json");
}

TEST_CASE("nothing resolvable yields an empty path, not a bogus one", "[adc]") {
	AdcPathInputs in;
	REQUIRE(ResolveAdcPath(in).empty());
}

// ---------------------------------------------------------------------------
// Document dispatch
// ---------------------------------------------------------------------------

TEST_CASE("an authorized_user document yields the refreshable triple", "[adc]") {
	// Shape taken from a real `gcloud auth application-default login` file.
	const std::string json = R"({
	  "account": "",
	  "client_id": "764086051850-6qr4p6gpi6hn506pt8ejuq83di341hur.apps.googleusercontent.com",
	  "client_secret": "cs-fake",
	  "quota_project_id": "my-project",
	  "refresh_token": "rt-fake",
	  "type": "authorized_user",
	  "universe_domain": "googleapis.com"
	})";

	auto parsed = ParseAdcJson(json);
	REQUIRE(parsed.ok);
	REQUIRE(parsed.kind == AdcKind::AUTHORIZED_USER);
	REQUIRE(parsed.user.client_id ==
	        "764086051850-6qr4p6gpi6hn506pt8ejuq83di341hur.apps.googleusercontent.com");
	REQUIRE(parsed.user.client_secret == "cs-fake");
	REQUIRE(parsed.user.refresh_token == "rt-fake");
	REQUIRE(parsed.user.quota_project_id == "my-project");
}

TEST_CASE("an authorized_user document missing the refresh token is rejected", "[adc]") {
	const std::string json = R"({
	  "client_id": "x.apps.googleusercontent.com",
	  "client_secret": "shhh",
	  "type": "authorized_user"
	})";

	auto parsed = ParseAdcJson(json);
	REQUIRE_FALSE(parsed.ok);
	// REQ-NF-03: the secret must not travel in the message.
	REQUIRE(parsed.error.find("shhh") == std::string::npos);
}

TEST_CASE("a service_account document is delegated to the key parser", "[adc]") {
	// Not a real key: ParseServiceAccountKey only checks structure here, and
	// signing (which would need a real RSA key) is a different module.
	const std::string json = R"({
	  "type": "service_account",
	  "project_id": "p",
	  "private_key_id": "kid",
	  "private_key": "not-a-real-key",
	  "client_email": "sa@p.iam.gserviceaccount.com"
	})";

	auto parsed = ParseAdcJson(json);
	REQUIRE(parsed.ok);
	REQUIRE(parsed.kind == AdcKind::SERVICE_ACCOUNT);
	REQUIRE(parsed.service_account.client_email == "sa@p.iam.gserviceaccount.com");
}

TEST_CASE("an external_account document is recognised and refused by name", "[adc]") {
	// Workload identity federation. We do not support it, but saying so beats
	// "missing client_id", which is what a naive parser would report.
	const std::string json = R"({"type": "external_account", "audience": "//iam.googleapis.com/x"})";

	auto parsed = ParseAdcJson(json);
	REQUIRE_FALSE(parsed.ok);
	REQUIRE(parsed.kind == AdcKind::EXTERNAL_ACCOUNT);
	REQUIRE(parsed.error.find("external_account") != std::string::npos);
}

TEST_CASE("a non-JSON document is rejected without echoing its content", "[adc]") {
	auto parsed = ParseAdcJson("this is not json, and here is a secret: hunter2");
	REQUIRE_FALSE(parsed.ok);
	REQUIRE(parsed.error.find("hunter2") == std::string::npos);
}

TEST_CASE("an OAuth client JSON is a distinct, named mistake", "[adc]") {
	// Downloading the wrong file from the Cloud Console is a common error;
	// "installed"/"web" wrappers are what an OAuth *client* looks like.
	const std::string json =
	    R"({"installed": {"client_id": "x", "client_secret": "SEKRIT"}})";

	auto parsed = ParseAdcJson(json);
	REQUIRE_FALSE(parsed.ok);
	REQUIRE(parsed.error.find("SEKRIT") == std::string::npos);
	REQUIRE(parsed.error.find("type") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The message that teaches the fix
// ---------------------------------------------------------------------------

TEST_CASE("the no-credentials message teaches the exact gcloud invocation", "[adc]") {
	const std::string msg = NoCredentialsMessage();

	// The command that actually works...
	REQUIRE(msg.find("gcloud auth application-default login") != std::string::npos);
	// ...the Drive scope, without which the user gets a 403 that reads like a
	// permissions problem (verified against the real API 2026-07-31)...
	REQUIRE(msg.find("https://www.googleapis.com/auth/drive") != std::string::npos);
	// ...and the alternatives, so the message is a map and not a dead end.
	REQUIRE(msg.find("service_account") != std::string::npos);
	REQUIRE(msg.find("CREATE SECRET") != std::string::npos);
}

TEST_CASE("the no-credentials message warns that plain gcloud auth login is not enough",
          "[adc]") {
	// The single most common mistake: `gcloud auth login` sets up the CLI, not
	// ADC, so the file never appears and the error looks like a bug in us.
	const std::string msg = NoCredentialsMessage();
	auto plain = msg.find("gcloud auth login");
	REQUIRE(plain != std::string::npos);
	REQUIRE(msg.find("not") < msg.size());
}
