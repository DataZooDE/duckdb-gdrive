// Cache identity material -- the rules that decide when two secrets may share
// cached path ids, blocks and tokens. Found incomplete by the final crew review.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "gdrive_identity.hpp"

using duckdb::gdrive::IdentityMaterial;
using Fields = std::vector<std::pair<std::string, std::string>>;

TEST_CASE("identity: a pasted ACCESS_TOKEN is the principal for PROVIDER config", "[identity]") {
	// A config secret recreated with another user's token must not inherit the
	// first user's cached ids and blocks.
	REQUIRE(IdentityMaterial("config", {{"access_token", "tok-A"}}, "") !=
	        IdentityMaterial("config", {{"access_token", "tok-B"}}, ""));
}

TEST_CASE("identity: authorization_code ignores the tokens it rewrites itself", "[identity]") {
	const Fields before = {{"client_id", "c"}, {"refresh_token", "r"}, {"access_token", "a1"}, {"expires_at", "1"}};
	const Fields after = {{"client_id", "c"}, {"refresh_token", "r"}, {"access_token", "a2"}, {"expires_at", "2"}};
	REQUIRE(IdentityMaterial("authorization_code", before, "") == IdentityMaterial("authorization_code", after, ""));
	// ...but a different user (refresh token) is a different identity.
	const Fields other = {{"client_id", "c"}, {"refresh_token", "r2"}, {"access_token", "a1"}, {"expires_at", "1"}};
	REQUIRE(IdentityMaterial("authorization_code", before, "") != IdentityMaterial("authorization_code", other, ""));
}

TEST_CASE("identity: the credential actually resolved is part of it", "[identity]") {
	// credential_chain has no user-supplied fields; service_account names a key
	// FILE whose content can change at the same path.
	REQUIRE(IdentityMaterial("credential_chain", {}, "/home/a/adc.json|120|1700000000") !=
	        IdentityMaterial("credential_chain", {}, "/home/b/adc.json|98|1700000000"));
	REQUIRE(IdentityMaterial("service_account", {{"key_file", "/k.json"}}, "/k.json|10|1") !=
	        IdentityMaterial("service_account", {{"key_file", "/k.json"}}, "/k.json|10|2"));
}

TEST_CASE("identity: field names are case-insensitive", "[identity]") {
	REQUIRE(IdentityMaterial("config", {{"ACCESS_TOKEN", "t"}}, "") ==
	        IdentityMaterial("config", {{"access_token", "t"}}, ""));
}
