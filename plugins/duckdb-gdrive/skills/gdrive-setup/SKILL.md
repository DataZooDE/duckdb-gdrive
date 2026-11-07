---
name: gdrive-setup
description: >
  Installing and authenticating the gdrive DuckDB extension: INSTALL/LOAD,
  CREATE SECRET (TYPE gdrive) with PROVIDER credential_chain (the default:
  gcloud Application Default Credentials, GOOGLE_APPLICATION_CREDENTIALS, a
  service-account key, or a workload identity federation external_account
  file), service_account (KEY_FILE), authorization_code (browser consent with
  your own OAuth client), config (ACCESS_TOKEN / REFRESH_TOKEN); DRIVE_ID (a
  Shared Drive) vs ROOT_FOLDER_ID (a folder), gdrive_drives() to find the id,
  DRIVE_SCOPE (never SCOPE); the gcloud --scopes trap, the Workspace "This app
  is blocked" page, why a service account can write only inside a Shared
  Drive; settings and gdrive_stats(). Use when connecting DuckDB to Google
  Drive, writing a CREATE SECRET, or fixing a 401/403 or "no secret
  configured" error.
version: 2026.10.18
user-invocable: false
---

# gdrive extension — setup and sign-in

```sql
INSTALL gdrive FROM community;
LOAD gdrive;
CREATE SECRET (TYPE gdrive);          -- credential_chain: your gcloud login
```

The login must ask for the Drive scope -- gcloud's default (`cloud-platform`)
does not include it, and every read then fails with 403:

```bash
gcloud auth application-default login --scopes=openid,https://www.googleapis.com/auth/drive
```

## Providers

| PROVIDER | when | fields |
|---|---|---|
| `credential_chain` (default) | your gcloud login, `GOOGLE_APPLICATION_CREDENTIALS`, a key file, or workload identity (`external_account`) -- no arguments | optional; `SET gdrive_adc_file = '<path>'` overrides discovery |
| `service_account` | unattended jobs | `KEY_FILE` |
| `authorization_code` | browser consent on this machine, your own *Desktop app* OAuth client | `CLIENT_ID`, `CLIENT_SECRET`, optional `REDIRECT_PORT` (8020) |
| `config` | a token you already have | `ACCESS_TOKEN`, or `REFRESH_TOKEN` + `CLIENT_ID` + `CLIENT_SECRET` |

Every provider also takes where `gdrive://` paths hang off -- `DRIVE_ID` for a
Shared Drive (id starts `0A`), **or** `ROOT_FOLDER_ID` for an ordinary folder
(not interchangeable: a folder id as `DRIVE_ID` answers 404 "Shared drive not
found") -- and `DRIVE_SCOPE` (default `drive.readonly`; use
`https://www.googleapis.com/auth/drive` to write).

```sql
CREATE SECRET sa (TYPE gdrive, PROVIDER service_account, KEY_FILE '/etc/creds/sa.json',
                  DRIVE_ID '0ABcDeFgHiJkLmNoPQ', DRIVE_SCOPE 'https://www.googleapis.com/auth/drive');
CREATE PERSISTENT SECRET me (TYPE gdrive, PROVIDER authorization_code,
                  CLIENT_ID '...apps.googleusercontent.com', CLIENT_SECRET '...',
                  DRIVE_SCOPE 'https://www.googleapis.com/auth/drive');
SELECT name, id FROM gdrive_drives();   -- the Shared Drives the identity is in; id -> DRIVE_ID
```

Rules that bite:
- The OAuth scope option is **`DRIVE_SCOPE`**. `SCOPE` is DuckDB's path-prefix list for
  the secret; a scope URL there makes the secret match no gdrive:// path ("no secret
  configured").
- A **service account has no storage quota**: it can read anything shared with it, but
  writes only inside a Shared Drive (else 403 `storageQuotaExceeded`).
- `DRIVE_SCOPE` cannot narrow a refresh token (`config`, gcloud logins): the scope is
  fixed at consent. Only service accounts and `authorization_code` enforce it.
- `authorization_code` keeps the refresh token in the secret: `CREATE PERSISTENT SECRET`
  to survive a restart (DuckDB's secret store, on disk).
- Credentials go in the secret, never in queries; `getenv('...')` works in CREATE SECRET.
- An `ATTACH`ed file sees only GLOBAL settings (`SET GLOBAL gdrive_...`).

## Errors
| error | fix |
|---|---|
| 403, "insufficient authentication scopes" / `ACCESS_TOKEN_SCOPE_INSUFFICIENT` | the token lacks the Drive scope: re-run gcloud with `--scopes=...auth/drive` |
| 403 `insufficientPermissions` (no scope detail) | the file is not shared with this identity |
| 403 `storageQuotaExceeded` | a service account writing outside a Shared Drive |
| "This app is blocked" in the browser | a Workspace domain blocks gcloud's client for restricted scopes: use a service-account key, have an admin trust the "Google Auth Library" client, or bring your own OAuth client (`--client-id-file`) |
| "requires a quota project" | the gcloud ADC file has no `quota_project_id`: `gcloud auth application-default set-quota-project <project>` |
| 404 "Shared drive not found" | a folder id passed as `DRIVE_ID`: use `ROOT_FOLDER_ID` |
| `'...' is ambiguous: N files share that name` | Drive allows duplicate names; use the `gdrive://id:<fileId>` the error names |

## Settings and diagnostics
`gdrive_permanent_delete` (false: removals go to the trash), `gdrive_docs_export_mime`
(`text/plain`), `gdrive_block_size_bytes` (16 MiB), `gdrive_block_cache_bytes` (256 MiB),
`gdrive_upload_chunk_bytes` (16 MiB), `gdrive_path_cache_entries` (4096),
`gdrive_immutable_prefixes`, `gdrive_adc_file`, `gdrive_telemetry_enabled`.
`SELECT * FROM gdrive_stats()` (Drive API calls by kind, cache hits, retries),
`CALL gdrive_reset_stats()`, `SELECT gdrive_version()`. `GDRIVE_TRACE_FILE=<path>`
writes one JSON line per API call.
