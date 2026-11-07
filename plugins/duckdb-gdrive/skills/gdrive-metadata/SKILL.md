---
name: gdrive-metadata
description: >
  Google Drive's metadata as tables with the gdrive DuckDB extension:
  read_gdrive_files (files under a path, glob or folder with owner, last
  modifier, size, md5, web_url, revision, custom properties), search_gdrive
  (Drive's full-text search; hits read back by id), gdrive_drives (Shared
  Drives and their ids), gdrive_versions and the ?version= path form (read an
  earlier revision with any reader), gdrive_changes (what changed under a
  folder since a token -- the mirror / incremental-sync pattern), and
  gdrive_permissions (who can reach what: direct, inherited, anyone-with-link,
  domain). Use when listing or inventorying Drive, finding a file, reading an
  old version, syncing a folder incrementally, or auditing sharing.
version: 2026.10.18
user-invocable: false
---

# gdrive extension — Drive's metadata as tables

Every function takes a `gdrive://` path (the scheme may be left out), an `id:` path or a
browser address. Table functions take no subqueries: pass a value through
`SET VARIABLE x = (...)` and `getvariable('x')`.

## Files with their metadata
```sql
SELECT name, size, modified, modified_by, owner_email, web_url
FROM read_gdrive_files('gdrive://Finance/**/*.csv');      -- a glob, a file, or a folder (its files)
```
Columns: `path, name, size, created, modified, modified_by(_email), owner(_email), mime_type,
id, web_url, version, md5, description, starred, shared, parent_id, properties`
(MAP of Drive's custom properties). A glob costs the listings `glob()` makes, no call per file.

## Search
```sql
SELECT name, path, modified FROM search_gdrive('quarterly forecast');
SELECT name, path FROM search_gdrive('', mime_type := 'application/vnd.google-apps.spreadsheet', max_results := 50);
SET VARIABLE p = (SELECT path FROM search_gdrive('plan 2026') LIMIT 1);
SELECT * FROM read_csv(getvariable('p'));
```
Full text over names and contents, everything the identity can see (its Shared Drive when
the secret binds `DRIVE_ID`). `path` is `gdrive://id:<id>`; same columns as read_gdrive_files.

## Shared Drives
`SELECT name, id FROM gdrive_drives()` (or `gdrive_drives(search := 'Fin')`) -- `id` is what
`DRIVE_ID` takes.

## Earlier versions
```sql
SELECT version, current, modified, modified_by, size, path FROM gdrive_versions('gdrive://f.csv');
SELECT * FROM read_csv('gdrive://f.csv?version=<revision id>');   -- any reader; read-only
```
Newest first; `path` reads that version. Uploaded files only: native Docs/Sheets list
revisions but cannot be read at one. Drive prunes old revisions unless `keep_forever`.
An in-place update (`write_blob`, `USE_TMP_FILE false`) makes a revision; `COPY`'s default
tmp-file-and-move makes a new file, with no history.

## What changed since last time
```sql
SELECT token FROM gdrive_changes('gdrive://Finance', token := 'latest');     -- just a token
SELECT * FROM gdrive_changes('gdrive://Finance', token := getvariable('t'));
```
Rows: `_change` = `upsert` | `delete` | `checkpoint` (exactly one, last; its `token` is for
next time), with `id, path, name, is_folder, mime_type, size, modified, modified_by(_email)`.
No token = everything under the folder once. A file trashed, deleted, or moved OUT of the
folder is a `delete` by `id` (path NULL when Drive no longer shows it under the folder).
Drive's feed lags writes by seconds. An unknown or expired token is an error: start over.
Mirror pattern: store the token FIRST, then the listing; each run delete the changed ids,
insert the upserts, store the new token (README "Keeping a mirror").

## Who can reach it
```sql
SELECT path, role, granted_by, grantee_type, grantee_email, link_scope, discoverable
FROM gdrive_permissions('gdrive://Shared/**') WHERE granted_by = 'link';
```
One row per permission: `role` (owner, organizer, fileOrganizer, writer, commenter, reader),
`granted_by` = `direct` | `inherited` | `link`; `link_scope` = `anonymous` (anyone with the
link) | `organization` (a domain), `discoverable` (findable in search), `link_url`,
`inherited_from` (Shared Drives), `expires`. A file or folder, or the files of a glob. A grant
no higher than an inherited one is not recorded by Drive and reads as inherited.
