#pragma once

#include <string>

namespace duckdb {

class ClientContext;

namespace gdrive {

// ---------------------------------------------------------------------------
// The seam between Wave 1 (auth) and Wave 2 (filesystem).
//
// Frozen before either wave fanned out, so the two could proceed in parallel
// against a fixed contract. The filesystem knows exactly
// one thing about authentication: how to obtain a bearer token for a secret.
// Everything else -- which provider minted it, whether it was refreshed on
// the way out, where the refresh token is persisted -- is Wave 1's business.
// ---------------------------------------------------------------------------

//! What the filesystem needs out of a resolved gdrive secret.
struct GDriveAuthContext {
	//! Bearer token, already refreshed if it had expired.
	std::string access_token;
	//! Optional Shared Drive binding. When set, paths resolve relative to
	//! this drive rather than the account root, and every API call is scoped
	//! to it (HLD section 4, mitigation 3).
	std::string drive_id;
	//! Optional folder id that gdrive:// resolves against, so a secret can
	//! pin queries to a subtree rather than the whole drive.
	std::string root_folder_id;
	//! The granted scope, so a permission error can name what is missing
	//! rather than saying "forbidden".
	std::string scope;
	//! The secret's name, for error messages that tell the user which
	//! credential to fix. NEVER accompanied by the token itself.
	std::string secret_name;
};

//! Resolve a secret to a usable token, refreshing transparently when expired
//! and writing new tokens back into the secret.
//!
//! `path` is the gdrive:// path being accessed; DuckDB's secret manager
//! matches secrets by scope prefix, so the path selects among several
//! configured secrets.
//!
//! Throws (DuckDB-side) when no secret matches or a refresh fails. The
//! message names the secret and what to do, and never contains token
//! material (REQ-NF-03).
GDriveAuthContext GetAuthContext(ClientContext &context, const std::string &path);

//! True when at least one gdrive secret is registered. Lets the filesystem
//! produce "no gdrive secret configured; CREATE SECRET ..." rather than an
//! opaque 401 from Google.
bool HasAnyGDriveSecret(ClientContext &context);

} // namespace gdrive
} // namespace duckdb
