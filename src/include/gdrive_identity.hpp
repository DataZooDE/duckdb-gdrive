#pragma once

#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gdrive {

//! The material a secret's cache identity (GDriveAuthContext::identity) is
//! fingerprinted from. PURE, so the rules are Catch2-tested:
//!
//!   * every secret field ("key=value"), except -- for PROVIDER
//!     authorization_code ONLY -- access_token and expires*/token_expiry,
//!     which that flow rewrites on every refresh (keeping them would drop the
//!     caches hourly). For PROVIDER config a pasted ACCESS_TOKEN IS the
//!     principal and stays in.
//!   * `identity_source`: what the provider actually authenticated with
//!     beyond the secret's own fields -- the key file's stamp (path, size,
//!     mtime) for service_account, the resolved ADC file's stamp for
//!     credential_chain -- so a credential replaced on disk is a new
//!     identity too.
std::vector<std::string> IdentityMaterial(const std::string &provider,
                                          const std::vector<std::pair<std::string, std::string>> &secret_fields,
                                          const std::string &identity_source);

} // namespace gdrive
} // namespace duckdb
