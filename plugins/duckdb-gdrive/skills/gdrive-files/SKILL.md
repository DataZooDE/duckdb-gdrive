---
name: gdrive-files
description: >
  Files in Google Drive with the gdrive DuckDB extension: the gdrive://
  filesystem (paths relative to the secret's root, the id: form, globs with
  * ? [a-z] ** {a,b}, duplicate-name errors), browser addresses pasted as
  paths (drive.google.com/file/d/..., docs.google.com/spreadsheets/d/...,
  folders), reading Parquet/CSV/JSON/anything through it, native Google
  Sheets and Docs (exported as CSV / text), COPY TO (streamed, appears on
  close), write_blob / remove_file / move_file / file_size, ATTACH of a
  .duckdb file (read-only), DuckLake with DATA_PATH on Drive, and the
  concurrency limits (no atomic create: concurrent writers duplicate). Use
  when reading or writing files in Google Drive from SQL, querying a Sheet,
  putting a lakehouse on Drive, or when the user pastes a Drive or Sheets link.
version: 2026.11.14
user-invocable: false
---

# gdrive extension — files

Paths are relative to the secret's root (`DRIVE_ID` / `ROOT_FOLDER_ID`, else My Drive):

```sql
SELECT * FROM 'gdrive://Finance/2026/actuals.parquet';         -- one files.list per path segment
SELECT * FROM read_csv('gdrive://id:1a2b3c...');                -- by file id: no resolution at all
SELECT * FROM read_parquet('gdrive://exports/**/part-[0-9]*.parquet');
COPY (SELECT * FROM t) TO 'gdrive://reports/out.parquet' (FORMAT parquet);  -- streamed, appears on close
SELECT write_blob('gdrive://notes/a.md', encode('# Hi'));       -- remove_file / move_file / file_size too
ATTACH 'gdrive://warehouse/sales.duckdb' AS s (READ_ONLY);      -- read-only, GLOBAL settings only
```

- Drive has **no path addressing**: `a/b/c.csv` costs three lookups (cached per secret).
  Generated or repeated queries should use `gdrive://id:<fileId>`.
- **Duplicate names are an error**, naming both ids: Drive allows two files of one name in
  a folder. Use the `id:` form. A glob returns such files as `gdrive://id:` paths.
- Names may contain glob characters; a pattern that matches nothing is tried literally.

## Browser addresses
The address bar or "Copy link" works wherever a path does -- it names the file by id:
```sql
SELECT * FROM read_csv('https://drive.google.com/file/d/1a2b3c.../view?usp=sharing');
SELECT * FROM read_csv('https://docs.google.com/spreadsheets/d/1x2y3z.../edit#gid=0');   -- a Sheet
SELECT * FROM read_gdrive_files('https://drive.google.com/drive/folders/1f2g3h...');     -- a folder
COPY t TO 'https://drive.google.com/file/d/1a2b3c.../view' (FORMAT csv);                 -- overwrites that file
```
Handled by this extension even with httpfs loaded (so it acts with the runner's secret).
`?...&version=<id>` in an address reads that version. Folder addresses work in the metadata
functions only, and nothing may follow the folder id. Writes need `/file/d/<id>/view` (or
`gdrive://id:<id>`); bare `/file/d/<id>`, `open?id=` and editor addresses are refused.
`web_url` from `read_gdrive_files` reads back.

## Native Sheets and Docs
They have no bytes, so they are exported once per open: Sheets as CSV (the **first tab**;
any tab or range, typed, and writing into a Sheet: the `gdrive-sheets` skill),
Docs as `text/plain` (`SET gdrive_docs_export_mime = 'text/markdown'`). Not cached across
queries -- materialise a Sheet you query repeatedly:
```sql
CREATE TABLE budget AS SELECT * FROM read_csv('gdrive://Finance/Budget');
```

## Writing
- Whole files only: sequential, streamed in bounded memory (`gdrive_upload_chunk_bytes`,
  16 MiB); positional writes and appends are refused. A failed `COPY` publishes nothing.
- Names are case-sensitive (`Report.csv` and `report.csv` are two files). A write never
  replaces a folder or a native Doc/Sheet; a trashed file is gone by path and by id.
- Overwrite keeps the old file until the new one lands; the replaced file goes to the trash
  (`SET gdrive_permanent_delete = true` to delete outright). `write_blob` updates in place
  (same id, a new revision -- see gdrive-metadata's versions).
- **No atomic create-if-absent.** Two writers to one path make two files of one name (the
  path is then ambiguous); concurrent writers into a NEW directory tree duplicate folders.
  Give each writer its own prefix (a table format does).
- Renames are not atomic: never put a writable `.duckdb` on Drive.

## DuckLake
Keep the catalog local, put only the data on Drive:
```sql
ATTACH 'ducklake:metadata.db' AS lake (DATA_PATH 'gdrive://lakehouse/');
SET gdrive_immutable_prefixes = 'gdrive://lakehouse';   -- skips a metadata call per file per query
```
`gdrive_immutable_prefixes` is a promise the files are never modified, replaced or
recreated in place; wrong, it causes stale reads nothing can detect.

## Cost
A ranged read costs ~1 s regardless of size, so reads go through a shared block cache
(`gdrive_block_size_bytes`, 16 MiB). A cold Parquet scan is about 3x object storage;
lookups, not bytes, dominate. `CALL gdrive_reset_stats(); <query>; SELECT * FROM gdrive_stats();`
shows the calls one query made.
