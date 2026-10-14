// Token freshness and the 401 policy -- the decisions a token SOURCE makes, so
// a long scan or an ATTACHed database can outlive the token it opened with.
// The minting itself (and the 401 it answers) is proven live in
// test/sql/gdrive_token_refresh.test.template; these are the pure rules.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>

#include "gdrive_token_policy.hpp"

using duckdb::gdrive::CachedTokenState;
using duckdb::gdrive::DecideAfterRejection;
using duckdb::gdrive::kTokenRefreshSlackSeconds;
using duckdb::gdrive::kUnknownExpiry;
using duckdb::gdrive::RejectionAction;
using duckdb::gdrive::ShouldResendAfter401;
using duckdb::gdrive::TokenIsFresh;

namespace {
CachedTokenState Token(const std::string &value, int64_t expires_at, int64_t minted_at = 0) {
	CachedTokenState t;
	t.access_token = value;
	t.expires_at_unix = expires_at;
	t.minted_at_unix = minted_at;
	return t;
}
} // namespace

TEST_CASE("token policy: a token is re-minted a minute BEFORE it expires", "[token]") {
	const int64_t now = 1000000;
	// A request that starts just before expiry must not race a token that dies
	// mid-flight.
	REQUIRE(TokenIsFresh(Token("a", now + kTokenRefreshSlackSeconds + 1), now, 0));
	REQUIRE_FALSE(TokenIsFresh(Token("a", now + kTokenRefreshSlackSeconds), now, 0));
	REQUIRE_FALSE(TokenIsFresh(Token("a", now - 1), now, 0));
}

TEST_CASE("token policy: a pasted token of unknown lifetime is trusted until Drive rejects it", "[token]") {
	const int64_t now = 1000000;
	REQUIRE(TokenIsFresh(Token("pasted", kUnknownExpiry, now - 86400), now, 0));
}

TEST_CASE("token policy: the debug max-age cap ages a token out early", "[token]") {
	const int64_t now = 1000000;
	const auto t = Token("a", now + 3000, /*minted_at=*/now - 2);
	// Cap 61 s: minted 2 s ago, so it has 59 s left -- inside the 60 s slack.
	REQUIRE_FALSE(TokenIsFresh(t, now, 61));
	// Cap 0 = Google's own lifetime.
	REQUIRE(TokenIsFresh(t, now, 0));
	REQUIRE(TokenIsFresh(t, now, 3600));
	// The cap applies to a pasted token too: otherwise the cap could never
	// exercise a seeded source's switch to minting.
	REQUIRE_FALSE(TokenIsFresh(Token("pasted", kUnknownExpiry, now - 2), now, 61));
}

TEST_CASE("token policy: after a 401, a NEWER cached token is reused, not re-minted", "[token]") {
	const int64_t now = 1000000;
	// Sixteen threads that all held the old token all get a 401. The first
	// mints; the other fifteen must find its token in the cache instead of
	// each POSTing to the token endpoint.
	REQUIRE(DecideAfterRejection(true, Token("new", now + 3600), "old", now, 0, true) ==
	        RejectionAction::REUSE_CACHED);
}

TEST_CASE("token policy: after a 401 on the cached token itself, mint", "[token]") {
	const int64_t now = 1000000;
	// Revoked or expired early: still "fresh" by its expiry, but Drive said no.
	REQUIRE(DecideAfterRejection(true, Token("tok", now + 3600), "tok", now, 0, true) == RejectionAction::MINT);
	// Nothing cached at all.
	REQUIRE(DecideAfterRejection(false, CachedTokenState(), "tok", now, 0, true) == RejectionAction::MINT);
	// A different cached token that is itself stale is no better.
	REQUIRE(DecideAfterRejection(true, Token("new", now + 10), "old", now, 0, true) == RejectionAction::MINT);
}

TEST_CASE("token policy: a source with nothing to refresh with gives up", "[token]") {
	const int64_t now = 1000000;
	// A pasted ACCESS_TOKEN alone: there is no refresh token, key or ADC file
	// to mint from, so the 401 is the answer.
	REQUIRE(DecideAfterRejection(false, CachedTokenState(), "pasted", now, 0, false) == RejectionAction::GIVE_UP);
	REQUIRE(DecideAfterRejection(true, Token("pasted", kUnknownExpiry), "pasted", now, 0, false) ==
	        RejectionAction::GIVE_UP);
}

TEST_CASE("token policy: a 401 is answered by at most ONE re-send, and only with a different token", "[token]") {
	REQUIRE(ShouldResendAfter401(/*already_refreshed=*/false, "old", "new"));
	// A second 401 is the answer, not a reason to loop.
	REQUIRE_FALSE(ShouldResendAfter401(true, "old", "new"));
	// Nothing to refresh with (the source returned no token).
	REQUIRE_FALSE(ShouldResendAfter401(false, "old", ""));
	// The same token again cannot succeed where it just failed.
	REQUIRE_FALSE(ShouldResendAfter401(false, "same", "same"));
}
