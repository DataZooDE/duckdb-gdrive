---
name: gdrive-metadata
description: >
  Google Drive's metadata as tables with the gdrive DuckDB extension:
  read_gdrive_files (files under a path, glob or folder with owner, last
  modifier, size, md5, web_url, revision, custom properties, lock), search_gdrive
  (Drive's full-text search; hits read back by id), gdrive_drives (Shared
  Drives and their ids), gdrive_versions and the ?version= path form (read an
  earlier revision with any reader), gdrive_changes (what changed under a
  folder since a token -- the mirror / incremental-sync pattern),
  gdrive_labels (Drive labels / classification on files, optional names), and
  gdrive_permissions (who can reach what: direct, inherited, anyone-with-link,
  domain). Use when listing or inventorying Drive, finding a file, reading an
  old version, syncing a folder incrementally, or auditing sharing or labels.
version: 2026.11.08
user-invocable: false
---

# gdrive extension — Drive's metadata as tables

New in v2026.11.08 (not in v2026.10.18 or earlier). Every function takes a
`gdrive://` path (the scheme may be left out), an `id:` path or a browser address -- of a
file, not a `?version=`; a literal path that names nothing is an error, a glob may match
nothing. Table functions take no subqueries: pass a value through
`SET VARIABLE x = (...)` and `getvariable('x')`.

## Files with their metadata
```sql
SELECT name, size, modified, modified_by, owner_email, web_url
FROM read_gdrive_files('gdrive://Finance/**/*.csv');      -- a glob, a file, or a folder (its files)
```
Columns: `path, name, size, created, modified, modified_by(_email), owner(_email), mime_type,
id, web_url, version, md5, description, starred, shared, parent_id, properties`
(MAP of Drive's custom properties), then the lock (Drive's content restriction):
`locked, locked_by, locked_by_email, locked_at, lock_reason, copy_requires_writer_permission`
(`locked_by_email` is NULL for a service account -- Drive hides it). A glob costs the listings
`glob()` makes, no call per file. Native Docs/Sheets: `size`, `md5`, `version` are NULL.
A write to a locked file is refused by Drive ("would violate a content restriction").
```sql
SELECT path, locked_by, locked_at, lock_reason FROM read_gdrive_files('gdrive://Finance/**') WHERE locked;
```

## Search
```sql
SELECT name, path, modified FROM search_gdrive('quarterly forecast');
SELECT name, path FROM search_gdrive('', mime_type := 'application/vnd.google-apps.spreadsheet', max_results := 50);
SET VARIABLE p = (SELECT path FROM search_gdrive('plan 2026') LIMIT 1);
SELECT * FROM read_csv(getvariable('p'));
```
Full text over names and contents, everything the identity can see (its Shared Drive when
the secret binds `DRIVE_ID`; `ROOT_FOLDER_ID` does NOT narrow it). Folders are hits too. `path` is `gdrive://id:<id>`; same columns as read_gdrive_files.

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
SELECT * FROM gdrive_changes('gdrive://Finance', token := getvariable('t'), outside := true);  -- for a mirror
```
Rows: `_change` = `upsert` | `delete` | `checkpoint` (exactly one, last; its `token` is for
next time), with `id, path, name, is_folder, mime_type, size, modified, modified_by(_email)`.
No token = everything under the folder once. A folder renamed or moved in brings its
subtree (new paths). Trashed or deleted = `delete` by `id` (a permanent delete has no path
and is always reported). Moved OUT looks like "never in": only `outside := true` reports
it (as path-less deletes, a folder with its subtree), and with it every change elsewhere
in the drive -- a mirror needs it, a "what's new here" query does not. Names shared by
siblings or containing '/' get `gdrive://id:` paths. One row per id. The feed lags writes
by seconds to minutes; a very old token replays everything since. An unknown or expired
token is an error: start over.
Mirror pattern: store the token FIRST, then the listing; each run (with `outside := true`)
delete the changed ids, insert the non-folder upserts, store the new token (README
"Keeping a mirror").

## Who can reach it
```sql
SELECT path, role, granted_by, grantee_type, grantee_email, link_scope, discoverable
FROM gdrive_permissions('gdrive://Shared/**') WHERE granted_by = 'link';
```
One row per permission: `role` (owner, organizer, fileOrganizer, writer, commenter, reader),
`granted_by` = `direct` | `inherited` | `link`; `link_scope` = `anonymous` (anyone with the
link) | `organization` (a domain), `discoverable` (findable in search), `link_url`,
`inherited_from` (Shared Drives), `expires`. A file or folder, or the files and folders of a
glob (not the glob's root: add it separately); one call per item. A grant
no higher than an inherited one is not recorded by Drive and reads as inherited.

## Labels (classification)
```sql
SELECT path, label_id, field_id, value FROM gdrive_labels('gdrive://Finance/**');
SELECT path, label, field, value FROM gdrive_labels('gdrive://Finance/**', names := true);
```
One row per (file, label, field, value): `path, id, label_id, label, field_id, field, value_type,
value` (`value_type` text | integer | dateString | selection | user; a user as their address).
A label with no field set: one row, `field_id` NULL. One `files.listLabels` per file.
`label`/`field` are NULL and a selection is its choice id unless `names := true`, which costs one
Drive Labels API call per query and needs the scope `.../auth/drive.labels.readonly` (service
account: add it to `DRIVE_SCOPE`, space-separated) AND the Drive Labels API enabled in the
credential's Cloud project -- each missing piece errors and says which. Labels need a Workspace
edition with labels enabled by the admin; where the identity may not read them, it errors rather
than reporting "unlabelled".
