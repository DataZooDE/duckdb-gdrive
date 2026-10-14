#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include <memory>
#include <string>

namespace duckdb {

class ClientContext;
class FileOpener;

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

// ---------------------------------------------------------------------------
// Where a bearer token comes from. A file handle can outlive the token it was
// opened with -- a long scan, an ATTACHed database kept open for hours -- so
// handles hold a SOURCE, not a token: every request asks it for a token that
// is valid now, and it re-mints shortly before expiry. The rules it follows
// are pure and unit-tested (gdrive_token_policy.hpp).
// ---------------------------------------------------------------------------
class GDriveTokenSource {
public:
	virtual ~GDriveTokenSource() = default;
	//! A token valid for at least the next minute, served from the process-wide
	//! token cache and minted when absent or about to expire. May throw (a
	//! DuckDB exception naming the secret) when minting fails.
	virtual std::string Token() = 0;
	//! After Drive answered 401 to `rejected`: the newer token another thread
	//! already obtained, else a freshly minted one -- or the EMPTY string when
	//! there is nothing to refresh with (a pasted ACCESS_TOKEN alone), in which
	//! case the 401 stands. THROWS a DuckDB exception naming the secret when a
	//! mint is attempted and fails (e.g. a revoked refresh token); that error
	//! replaces the 401, since it says what to fix. A failed mint is
	//! remembered for 30 s and rethrown without contacting the token endpoint.
	virtual std::string Refresh(const std::string &rejected) = 0;
};

//! What the filesystem needs out of a resolved gdrive secret.
struct GDriveAuthContext {
	//! Supplies `Authorization: Bearer <token>` for each request. Shared by
	//! every client built from this context, and by the handle that keeps it.
	std::shared_ptr<GDriveTokenSource> tokens;
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
	//! Cache identity: a SHA-256 fingerprint of the secret's provider, name,
	//! fields and the credential actually resolved (key file / ADC file stamp)
	//! -- see gdrive_identity.hpp for the exact, unit-tested rules. The path cache and the block cache
	//! key on THIS, never on the name alone: a secret recreated under the same
	//! name with different credentials must not be served the previous
	//! principal's cached ids or bytes.
	std::string identity;
	//! GCP project to bill Drive API quota to, sent as `x-goog-user-project`.
	//!
	//! Only ever set for an `authorized_user` ADC document, and REQUIRED
	//! there: Drive refuses a user credential that names no quota project
	//! with "the drive.googleapis.com API requires a quota project, which is
	//! not set by default". That is a hard failure on every call, not a
	//! billing nicety, so a `credential_chain` secret over a gcloud login is
	//! unusable without it. `gcloud auth application-default login` writes
	//! the project into the ADC file as `quota_project_id`.
	//!
	//! Empty for service accounts, which carry their own project association
	//! and need no header.
	std::string quota_project_id;
};

//! Resolve a secret to a token source, obtaining a first token eagerly (so a
//! bad credential fails here, naming the secret, rather than on a later read)
//! and writing new tokens back into the secret where the provider does.
//!
//! `path` is the gdrive:// path being accessed; DuckDB's secret manager
//! matches secrets by scope prefix, so the path selects among several
//! configured secrets.
//!
//! Throws (DuckDB-side) when no secret matches or a refresh fails. The
//! message names the secret and what to do, and never contains token
//! material (REQ-NF-03).
GDriveAuthContext GetAuthContext(optional_ptr<FileOpener> opener, const std::string &path);

//! True when at least one gdrive secret is registered. Lets the filesystem
//! produce "no gdrive secret configured; CREATE SECRET ..." rather than an
//! opaque 401 from Google.
bool HasAnyGDriveSecret(optional_ptr<FileOpener> opener);

} // namespace gdrive
} // namespace duckdb
