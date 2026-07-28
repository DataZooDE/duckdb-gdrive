#pragma once

#include "gdrive_client.hpp"

#include <cstdint>

#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// New (not frozen) header, owned by src/gdrive_client.cpp + src/gdrive_stats.cpp
// (S-2.2 / S-2.5 / part of S-4.1). It exists to give other tracks exactly two
// things without requiring an edit to the frozen src/include/gdrive_client.hpp:
//
//   1. CreateGDriveClient()   -- the factory GDriveFileSystem/resolver call to
//                                obtain a GDriveClient for a resolved bearer
//                                token (see gdrive_auth.hpp's GDriveAuthContext).
//   2. RegisterGDriveStats()  -- called once from gdrive_extension.cpp's
//                                LoadInternal() to register the gdrive_stats()
//                                table function. That file is owned by another
//                                track; it just needs to `#include
//                                "gdrive_stats.hpp"` and add one call.
//
// Also exposes the process-wide DriveCallStats aggregate that gdrive_stats()
// reads from. GDriveClientImpl mirrors every counter it increments into this
// registry (in addition to its own per-instance copy, which satisfies the
// frozen GDriveClient::Stats()/ResetStats() contract). Components that are
// not an GDriveClient -- e.g. the resolver's path cache -- may report their
// own cache_hits/cache_misses here directly.
// ---------------------------------------------------------------------------

namespace duckdb {

class ExtensionLoader;

namespace gdrive {

//! Snapshot of the process-wide aggregate (NOT reset by any one
//! GDriveClientImpl's ResetStats() -- see gdrive_client.cpp for the rationale).
DriveCallStats GetGlobalDriveCallStats();
void ResetGlobalDriveCallStats();

//! Live size of the path cache. A GAUGE, not a counter: gdrive_stats() is a
//! global table function with no handle on the filesystem object, and the
//! cache is the only thing that knows its own size. uint64_t rather than
//! idx_t because this header is reachable from the pure sources, which must
//! not pull in duckdb.hpp.
void SetGlobalPathCacheEntries(uint64_t entries);
uint64_t GetGlobalPathCacheEntries();

//! For components that track Drive-related activity but are not themselves a
//! GDriveClient (the path-resolution cache, notably).
void IncrementGlobalCacheHit();
void IncrementGlobalCacheMiss();

//! The filesystem's one way to obtain a transport. `access_token` is a bearer
//! token already resolved (and refreshed if needed) via gdrive_auth.hpp's
//! GetAuthContext(). `drive_id`, when non-empty, scopes every files.list to
//! that Shared Drive (corpora=drive, per HLD section 4 mitigation 3 / D-5).
std::unique_ptr<GDriveClient> CreateGDriveClient(const std::string &access_token, const std::string &drive_id = "");

//! Registers gdrive_stats() -- one row per counter (metric VARCHAR, value
//! BIGINT) -- reading GetGlobalDriveCallStats(). Call once from
//! gdrive_extension.cpp's LoadInternal(); not called from this file.
void RegisterGDriveStats(ExtensionLoader &loader);

} // namespace gdrive
} // namespace duckdb
