# HLD — `duckdb-gdrive`

**Status:** Draft. Companion to `brd.md`.
**Date:** 2026-07-26
**Target:** DuckDB v1.5.x

---

## 1. Architecture

DuckDB routes every file access through a virtual filesystem. Extensions add
schemes by subclassing `duckdb::FileSystem` and registering the subclass; the
core then dispatches any path that subclass claims.

`duckdb-gdrive` is one such subclass — `GDriveFileSystem`, claiming
`gdrive://` — translating DuckDB's byte-oriented, path-addressed calls into
Google Drive API v3 calls, which are fileId-addressed and HTTP-ranged.

Everything layered on DuckDB's filesystem inherits the scheme without knowing
it exists: `read_parquet`, `read_csv`, `COPY`, `glob`, `ATTACH`, and table
formats such as DuckLake whose data files and cleanup routines go through the
same interface. That inheritance is the reason to implement at this layer
rather than inside any single consumer.

```
  read_parquet('gdrive://Finance/2026/actuals.parquet')
            │
            ▼
  duckdb::FileSystem::CanHandleFile("gdrive://…")  ──► GDriveFileSystem
            │
   ┌────────┴──────────┬─────────────────┬──────────────────┐
   ▼                   ▼                 ▼                  ▼
 PathResolver      RangeReader       Uploader          SecretProvider
 (path→fileId,     (GET alt=media,   (resumable        (CREATE SECRET
  cached)           Range: bytes=)    upload)           TYPE gdrive)
   │                   │                 │                  │
   └───────────────────┴────────┬────────┴──────────────────┘
                                ▼
                     Google Drive API v3 (HTTPS)
```

---

## 2. The extension point

From `duckdb/src/include/duckdb/common/file_system.hpp`:

```cpp
DUCKDB_API virtual void RegisterSubSystem(unique_ptr<FileSystem> sub_fs);   // :267
DUCKDB_API virtual bool CanHandleFile(const string &fpath);                 // :281
DUCKDB_API virtual vector<OpenFileInfo> Glob(const string &path, ...);      // :258
```

Registration happens once, in the extension's load entrypoint:

```cpp
db.GetFileSystem().RegisterSubSystem(make_uniq<GDriveFileSystem>());
```

---

## 3. Method mapping

The interface obligation is larger than the happy path suggests:

| DuckDB method | Drive API | Notes |
|---|---|---|
| `CanHandleFile(path)` | — | `path.starts_with("gdrive://")` |
| `OpenFile(path, flags)` | `files.get` (metadata) | Resolves path→fileId; caches size, `modifiedTime`, `headRevisionId`, `mimeType` on the handle |
| `Read(h, buf, n, location)` | `files.get?alt=media` + `Range: bytes=…` | **The hot path.** Drive honours HTTP Range |
| `Read(h, buf, n)` | as above | Sequential; handle tracks the pointer |
| `Write(h, buf, n)` | buffered locally | Nothing reaches Drive until close |
| `Write(h, buf, n, location)` | — | **`NotImplementedException`.** Drive cannot write at an offset; failing loudly beats corrupting silently |
| `GetFileSize(h)` | cached from `OpenFile` | |
| `GetLastModifiedTime(h)` | cached | `modifiedTime` |
| `GetVersionTag(h)` | `headRevisionId` | Core documents this as the cache-invalidation tag used by the HTTP filesystem's caching layer. Drive's revision id fits exactly, so caching integrates properly instead of being bolted on |
| `FileExists(path)` | `files.list` name query | Via resolver |
| `ListFiles(dir, cb)` | `files.list?q='<parentId>' in parents` | Paginated |
| `Glob(path)` | `files.list` + local pattern match | Needed for `gdrive://folder/*.parquet` |
| `RemoveFile(path)` | `files.delete` or trash | See §8 |
| `MoveFile(src, dst)` | `files.update` (parents/name) | Core notes the storage manager relies on rename atomicity; Drive does not guarantee it — **document the limitation** |
| `CreateDirectory` / `DirectoryExists` / `RemoveDirectory` | `files.create` with folder mimeType | Drive folders are files with a folder mimeType |
| `Truncate`, `Trim` | — | `NotImplementedException` |
| `GetName()` | — | `"GDriveFileSystem"` |

**Design consequence:** the read path is genuinely well matched — ranged GETs
over effectively immutable blobs. The write path is not, and the honest answer
is a restricted one (§7).

---

## 4. Path model and the resolver

This is the core problem. Drive has **no path addressing**;
`gdrive://Finance/2026/actuals.parquet` must be walked into a fileId:

```
root (or Shared Drive id)
  └─ files.list?q="'<root>' in parents and name='Finance'"        → folderId₁
      └─ files.list?q="'<folderId₁>' in parents and name='2026'"  → folderId₂
          └─ files.list?q="'<folderId₂>' in parents and name='actuals.parquet'" → fileId
```

Three round trips before a single byte is read. This is risk R-1 and the most
likely thing to sink the project.

**Mitigations, in order:**

1. **Per-connection resolver cache** keyed on path prefix, so sibling files in
   a folder cost one lookup for the folder rather than one per file.
2. **Direct-fileId form** — `gdrive://id:<fileId>` bypasses resolution
   entirely. Cheap to implement, and the form generated or stored queries
   should use.
3. **Root binding in the secret**, so paths resolve relative to a configured
   folder or Shared Drive rather than the account root.

**Name collisions (R-4).** Drive permits duplicate names in one folder. The
rule: **more than one match is an error**, naming both file ids and suggesting
the `id:` form. Silently choosing one would make query results depend on
Drive's internal ordering.

---

## 5. Authentication

**Most of this is already built.** The `erpl-web` extension contains a
complete, provider-agnostic OAuth2 client — written for SAP, then generalised
to Microsoft Entra. Google is the third provider, and the generalisation hooks
it needs already exist.

### 5.1 What moves into the library, unchanged

These are lifted into `datazoo-oauth2` (§5.4) with no logic changes. Source
paths are their current homes in `erpl-web`.

| Component | Current home | Provides |
|---|---|---|
| `OAuth2Config` | `oauth2_types.hpp` | grant type, client id/secret, scope, redirect URI, **and `custom_auth_url` / `custom_token_url` overrides** — the hook added so a non-SAP provider could be plugged in |
| `OAuth2Tokens` | `oauth2_types.hpp` | access/refresh tokens, `IsExpired()`, `NeedsRefresh()`, `CalculateExpiresAfter()` |
| `OAuth2Utils` | `oauth2_types.hpp` | PKCE `GenerateCodeVerifier()` / `GenerateCodeChallenge()` — RFC 7636, no provider specifics |
| `OAuth2FlowV2` | `oauth2_flow_v2.hpp` | the whole authorization-code flow: build URL → browser → exchange code → parse token response |
| `OAuth2Server` | `oauth2_server.hpp` | loopback HTTP server catching the redirect; `StartAndWaitForCode(expected_state, port)` |
| `OAuth2Browser` | `oauth2_browser.hpp` | cross-platform browser launch (Windows/macOS/Linux) + free-port discovery |
| `OAuth2CallbackHandler` | `oauth2_callback_handler.hpp` | state/CSRF validation, timeout, error surfacing |
| HTTP client | `http_client.hpp` | shared client, including timeout tuning already learned against identity endpoints |

Between them these cover RFC 6749 authorization-code and RFC 7636 PKCE end to
end. Google requires exactly that flow, so the interactive path is close to
configuration rather than implementation.

### 5.2 What moves after generalisation

The Microsoft Entra secret (`microsoft_entra_secret.hpp`) is the template, but
it is Entra-shaped and must be generalised on the way in (§5.4):

- **Three creation providers** on one secret type — `client_credentials`,
  `config` (paste pre-obtained tokens), `authorization_code` (interactive).
  This maps one-to-one onto REQ-F-05.
- **`MicrosoftEntraTokenManager` → `OAuth2SecretTokenManager`** in the library,
  with the Entra token URL and client-credentials shape lifted out as inputs.
  The valuable part: `GetToken(context, secret)` returns a usable token,
  refreshing transparently when expired and writing the new tokens back into
  the secret. This is the difference between OAuth that works once and OAuth
  that keeps working, and it is the single highest-value thing to extract.
  Each extension then registers its own concrete secret type on top.
- **`RedactCommonKeys`** — keeps tokens out of `duckdb_secrets()` output,
  satisfying REQ-NF-03 by construction rather than by discipline.

Resulting shape:

```sql
-- interactive: browser flow, refresh token stored in the secret
CREATE SECRET gdrive_user (
    TYPE gdrive, PROVIDER authorization_code,
    CLIENT_ID '…', CLIENT_SECRET '…',
    SCOPE 'https://www.googleapis.com/auth/drive.readonly'
);

-- unattended: service account
CREATE SECRET gdrive_sa (
    TYPE gdrive, PROVIDER service_account,
    KEY_FILE '/etc/creds/sa.json',
    SCOPE 'https://www.googleapis.com/auth/drive.readonly'
);
```

### 5.3 Genuinely new work

1. **Endpoints** — `https://accounts.google.com/o/oauth2/v2/auth` and
   `https://oauth2.googleapis.com/token`, set through the existing
   `custom_auth_url` / `custom_token_url` fields. Effectively free.

2. **`access_type=offline` + `prompt=consent`** — Google will not issue a
   refresh token without them. `BuildAuthorizationUrl` currently composes a
   fixed parameter set, so this needs a small extra-parameters hook. **It is
   the one change required to shared code**, and it should be made generically
   (a provider-supplied parameter map) rather than by branching on Google.

3. **Service-account JWT flow (RFC 7523)** — the only substantial new
   authentication code. Google's server-to-server flow is not Microsoft's
   client-credentials POST: it signs a JWT assertion with the service
   account's private key and posts it with
   `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer`. Needs RS256
   signing; `jwt-cpp` is already an established dependency in the in-house
   extension scaffold. Estimate 2–3 days.

4. **Scope strings** — `drive.readonly`, `drive.file`, `drive`; default to the
   narrowest the configuration needs (REQ-NF-04).

### 5.4 `datazoo-oauth2` — the shared library (REQ-A-01/02)

The reusable code is **extracted into a standalone library that both
extensions consume**. Vendoring a copy is rejected: it is the cheapest option
on day one and the most expensive by month six, because it silently creates a
second OAuth implementation to fix bugs in twice.

#### Boundary

| In the library | Stays in the consuming extension |
|---|---|
| `OAuth2Config`, `OAuth2Tokens` | endpoint URLs, scope strings |
| PKCE generation (RFC 7636) | secret type name and its providers |
| `OAuth2Flow` — build URL, exchange code, parse response | extra authorization parameters (Google's `access_type`/`prompt`) |
| `OAuth2Server` — loopback redirect catcher | provider-specific grants (Entra client-credentials; Google SA JWT) |
| `OAuth2Browser` — cross-platform launch, port discovery | anything naming a vendor |
| `OAuth2CallbackHandler` — state/CSRF, timeout | |
| `OAuth2SecretTokenManager` — get-token-with-refresh against a DuckDB secret, generalised from `MicrosoftEntraTokenManager` | the concrete secret registration |

REQ-A-03 makes this testable: **grep the library for a vendor name; a hit is a
design failure.**

#### Two deliberate couplings

The library is **not** a general-purpose C++ OAuth client, and pretending
otherwise would over-engineer it:

- It depends on **DuckDB** — `ClientContext`, `KeyValueSecret`, the secret
  manager — because the token manager's whole value is persisting refreshed
  tokens back into a DuckDB secret.
- It depends on DuckDB's **vendored httplib** for the loopback server and
  token requests. Both consumers are DuckDB extensions, so this costs nothing
  and avoids inventing an HTTP abstraction with two implementations.

Stated plainly so nobody later "fixes" it into a generic library with an
injectable transport. Its scope is: *OAuth2 for DuckDB extensions.*

#### Two generalisations required during extraction

The code is provider-agnostic in most places, but not all. Both changes are
behaviour-preserving for `erpl-web`:

1. **Extra authorization parameters.** `BuildAuthorizationUrl` composes a fixed
   set. Google needs `access_type=offline` and `prompt=consent` or it issues no
   refresh token. Add a `map<string,string> extra_auth_params` on the config —
   generic, not a Google branch. Empty for `erpl-web`, so no change in
   behaviour.
2. **Token manager generalisation.** `MicrosoftEntraTokenManager` hard-codes
   Entra's token URL and client-credentials shape. Extract the
   get-token → check-expiry → refresh → write-back-to-secret logic, taking the
   token URL and a refresh-request builder as inputs.

#### Consumption

Git submodule + CMake `add_subdirectory`, matching how `extension-ci-tools` is
already consumed. Each extension **pins the library by commit**, so neither is
forced to upgrade on the other's schedule (R-9).

#### Migration order — non-negotiable

`erpl-web` is a shipping extension. The sequence protects it:

1. Create `datazoo-oauth2`; move the code with **no logic changes**; add the
   library's own tests (REQ-A-04).
2. Point `erpl-web` at the library; delete its copies. **Gate: `erpl-web`'s
   existing OAuth2 suites — `test_datasphere_oauth2_consolidated.cpp` and
   `test_microsoft_entra_auth.cpp` — pass unchanged.** Unchanged is the point:
   if a test needs editing, the extraction was not behaviour-preserving and
   should be reworked, not the test.
3. Only then build the Google provider on top.

Step 2 is the acceptance gate for REQ-A-02, and the extraction is not done
until it is green. Do not start step 3 early to save time — a half-migrated
`erpl-web` is exactly the fork this requirement exists to prevent.

### 5.5 Non-negotiables

Per REQ-NF-03: tokens live in memory or in DuckDB secrets, are never written
to disk by this extension, and never appear in error text. Credential files
must never be committed; CI should fail on any file matching Google's key-file
patterns.

---

## 6. Read path

1. `OpenFile` resolves the path (§4); one `files.get` returns `size`,
   `modifiedTime`, `headRevisionId` and `mimeType`.
2. `Read(h, buf, n, location)` issues `GET files/{id}?alt=media` with
   `Range: bytes=location-(location+n-1)`.
3. `GetVersionTag` returns `headRevisionId`, letting DuckDB's caching layer
   reuse blocks across statements and invalidate correctly when the file
   changes in Drive.

**Native Google formats (REQ-F-07).** When `mimeType` is
`application/vnd.google-apps.*` the file has no bytes and `alt=media` fails.
Use `files.export` with a target type instead (Sheet → `text/csv`,
Doc → `text/plain` or `text/markdown`). Export **does not support Range**, so
the whole export is fetched once and cached on the handle, and `GetFileSize`
reports the exported length rather than `size`, which is absent for native
files. This is a genuine behavioural fork and needs dedicated tests.

---

## 7. Write path

Deliberately restricted:

- sequential `Write` only; positional throws;
- bytes accumulate in a local temp buffer;
- on close, one resumable upload (`files.create` or `files.update`);
- no concurrent multi-part upload in v1.

This suits engines that emit whole immutable files. It does not suit anything
expecting random-access writes, and the error message says so explicitly.

---

## 8. Deletion semantics — an explicit decision

`files.delete` is permanent; moving to trash is recoverable. DuckDB's
`RemoveFile` contract implies deletion. Table-format cleanup routines delete
superseded data files, so "permanent" is the technically correct mapping — and
the more alarming default on a user's own Drive.

**Proposal:** default to **trash**, with `gdrive_permanent_delete=true` to opt
into hard deletion. Flagged for review: this is a judgement call about user
expectation, not a technical constraint.

---

## 9. Error mapping (REQ-F-08)

| Drive response | DuckDB exception |
|---|---|
| 404 | `IOException` — "no such file", naming the resolved path |
| 401 / expired | `IOException` — names the secret and that it needs re-auth |
| 403 `insufficientPermissions` | `PermissionException` — names the required scope |
| 403 / 429 rate-limit | `IOException` — explicitly "Drive API quota", with retry-after. **Must not** read as a generic failure (R-2) |
| 5xx | retried with jittered backoff, then `IOException` |

---

## 10. Testing

- **Unit** — path parsing, collision handling, Range header construction, error
  mapping. No network.
- **Integration against a fake** — a local HTTP server speaking enough of Drive
  v3 to exercise read/list/glob/write deterministically. This is what makes the
  suite runnable in CI without credentials, and is a real build item rather
  than an afterthought.
- **Live, opt-in** — against a real Drive service account behind a feature
  flag: scheduled and on-demand, never on the merge path.
- **Benchmark, gating** — the Milestone-1 comparison against a FUSE mount and
  against object storage for the same file, reported as a committed number.

---

## 11. Build and release

Standard DuckDB extension layout: `CMakeLists.txt`, `extension_config.cmake`,
`vcpkg.json`, and `extension-ci-tools` as a submodule for the platform matrix.

New dependencies: an HTTP client, a JSON parser, and JWT signing for the
service-account flow (§5.3). All three are already established in in-house
DuckDB extensions, so the vcpkg manifest is a known quantity rather than new
ground.

Distribution through the DuckDB community extensions repository, so
`INSTALL gdrive; LOAD gdrive;` works — which requires public source and
acceptance of the community review process (§13).

---

## 12. Milestones

| # | Deliverable | Gate |
|---|---|---|
| **M0** | **`datazoo-oauth2` extraction + `erpl-web` migration** (§5.4, REQ-A-01/02). Behaviour-preserving move, library tests added, `erpl-web` consumes it. | **`erpl-web`'s existing OAuth2 suites pass unchanged.** Independently valuable — survives M1 killing the rest |
| **M1** | **Throwaway read-only spike.** Resolve a path, ranged-read a Parquet, run a real query. Benchmark against a FUSE mount and against object storage. Measure metadata amplification (R-1). | **Decision point.** If the mount is comparable, stop. |
| M2 | Read path proper: resolver + cache, `Glob`/`ListFiles`, error mapping, fake-server suite | Cold Parquet scan within 3× object storage (REQ-NF-01) |
| M2a | Auth: Google provider on top of the library — endpoints, extra params, secret with three providers | Interactive browser flow yields a stored refresh token that survives restart |
| M2b | Service-account JWT flow (RFC 7523) | Unattended auth with no browser |
| M3 | Native-format export (Sheets/Docs) | A Sheet is queryable with no manual export |
| M4 | Write path (sequential, buffered) | `COPY … TO 'gdrive://…'` round-trips |
| M5 | CI matrix, docs, community-extension submission | Green on all target platforms |

M1 is ≈1 week and is the only milestone the BRD recommends funding up front.
**M0 can run independently of that gate** — it stands on its own merits for
`erpl-web` whether or not the Drive extension proceeds, which is why it is
sequenced first rather than bundled into M2.

---

## 13. Open questions

1. **Does a FUSE mount already solve this well enough?** M1 must answer it.
   Nothing else matters until it does.
2. **Who owns `datazoo-oauth2`?** The library needs an owner distinct from
   "whoever last touched a consumer", or it decays into the fork REQ-A-02
   exists to prevent (R-9).
3. **Does `datazoo-oauth2` ship public or private?** It must at least match
   the most permissive consumer; see Q7.
4. **Trash or permanent delete** as the `RemoveFile` default (§8)?
5. **Shared Drives** in v1, or personal Drive only? Affects the resolver root
   and the secret shape.
6. **Which export type for Docs** — `text/markdown` or `text/plain`? Markdown
   preserves structure but is not byte-stable across exports, which makes
   caching and diffing noisier.
7. **Public or private source?** Community-extension distribution requires
   public. If it must stay private, users have to build it themselves, which
   materially reduces the value.
