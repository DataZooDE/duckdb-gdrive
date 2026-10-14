// See gdrive_token_policy.hpp. PURE.
#include "gdrive_token_policy.hpp"

#include <cstdint>

namespace duckdb {
namespace gdrive {

bool TokenIsFresh(const CachedTokenState &token, int64_t now_unix, int64_t max_age_seconds) {
	const int64_t horizon = now_unix + kTokenRefreshSlackSeconds;
	if (token.expires_at_unix <= horizon) {
		return false;
	}
	// Under the test cap a token minted before the cap was set counts too:
	// age is measured from minting, not from when the cap appeared.
	return max_age_seconds <= 0 || token.minted_at_unix + max_age_seconds > horizon;
}

RejectionAction DecideAfterRejection(bool has_cached, const CachedTokenState &cached, const std::string &rejected,
                                     int64_t now_unix, int64_t max_age_seconds, bool can_mint) {
	if (has_cached && cached.access_token != rejected && TokenIsFresh(cached, now_unix, max_age_seconds)) {
		return RejectionAction::REUSE_CACHED;
	}
	return can_mint ? RejectionAction::MINT : RejectionAction::GIVE_UP;
}

bool ShouldResendAfter401(bool already_refreshed, const std::string &rejected, const std::string &replacement) {
	return !already_refreshed && !replacement.empty() && replacement != rejected;
}

} // namespace gdrive
} // namespace duckdb
