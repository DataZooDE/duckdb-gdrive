#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// The decisions a token SOURCE makes. PURE: no duckdb.hpp, no I/O.
//
// A file handle can outlive the token it was opened with -- a long scan, or
// an ATTACHed database kept open for hours -- so handles hold a source (see
// GDriveTokenSource in gdrive_auth.hpp), not a token. Every request asks the
// source for a token that is valid now; a 401 is answered by ONE refresh and
// one re-send. These functions are the rules; gdrive_auth.cpp does the
// minting and gdrive_client.cpp the re-send.
// ---------------------------------------------------------------------------

//! Re-mint when fewer than this many seconds remain, not exactly at expiry: a
//! request that starts just before expiry must not race a token that dies
//! mid-flight.
constexpr int64_t kTokenRefreshSlackSeconds = 60;

//! Expiry of a token whose lifetime we were never told -- a pasted
//! ACCESS_TOKEN. Trusted until Drive rejects it (or the debug cap ages it out).
constexpr int64_t kUnknownExpiry = std::numeric_limits<int64_t>::max();

struct CachedTokenState {
	std::string access_token;
	//! Unix seconds; kUnknownExpiry for a pasted token.
	int64_t expires_at_unix = 0;
	//! Unix seconds; only consulted under the debug max-age cap.
	int64_t minted_at_unix = 0;
};

//! Usable for at least another kTokenRefreshSlackSeconds -- by its own expiry
//! and, when `max_age_seconds` > 0 (the gdrive_debug_token_max_age_seconds
//! test hook), by age as well. 0 means Google's own lifetime.
bool TokenIsFresh(const CachedTokenState &token, int64_t now_unix, int64_t max_age_seconds);

//! Validate the gdrive_debug_token_max_age_seconds setting. False for a
//! negative value (the caller rejects it). 0 (no cap) and caps above the
//! refresh slack pass through; 1..kTokenRefreshSlackSeconds are clamped to
//! kTokenRefreshSlackSeconds + 1, because any smaller cap makes a token stale
//! the moment it is minted -- one token request per Drive request.
bool NormalizeMaxAgeSeconds(int64_t requested, int64_t &effective);

enum class RejectionAction {
	//! Another thread already replaced the rejected token; use that one.
	REUSE_CACHED,
	//! Discard the cached token and mint a new one.
	MINT,
	//! Nothing to refresh with (a pasted ACCESS_TOKEN alone): the 401 stands.
	GIVE_UP,
};

//! After Drive answered 401 to `rejected`. `cached` is what the process-wide
//! token cache holds for this credential now (`has_cached` false: nothing).
//! Decided under the credential's mint lock, so N threads rejected at once
//! mint ONE token: the first mints, the rest find it here.
RejectionAction DecideAfterRejection(bool has_cached, const CachedTokenState &cached, const std::string &rejected,
                                     int64_t now_unix, int64_t max_age_seconds, bool can_mint);

//! Whether a request that got a 401 with `rejected` is re-sent with
//! `replacement`. At most once per request -- a second 401 is a clean
//! authentication error, not a loop -- and only with a token that is actually
//! different (empty means the source had nothing to refresh with).
bool ShouldResendAfter401(bool already_refreshed, const std::string &rejected, const std::string &replacement);

} // namespace gdrive
} // namespace duckdb
