<h1 align="center">duckdb-gdrive</h1>
<p align="center"><strong>Google Drive in SQL: files, Sheets, and Drive's own metadata for DuckDB</strong></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT"></a>
  <a href="https://duckdb.org"><img src="https://img.shields.io/badge/DuckDB-1.5.6%20%7C%202.0-green.svg" alt="DuckDB 1.5.6 | 2.0"></a>
  <a href="https://duckdb.org/community_extensions/extensions/gdrive"><img src="https://img.shields.io/badge/DuckDB-community%20extension-yellow.svg" alt="DuckDB community extension"></a>
  <a href="#claude-code-plugin"><img src="https://img.shields.io/badge/Claude_Code-AI_skills_plugin-8A2BE2.svg" alt="Claude Code AI skills plugin"></a>
  <a href="https://github.com/DataZooDE/duckdb-gdrive/actions/workflows/MainDistributionPipeline.yml"><img src="https://github.com/DataZooDE/duckdb-gdrive/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=main" alt="Build"></a>
</p>

**Google Drive in SQL.** Query the Parquet drops, CSV exports and Sheets your
team keeps in Drive directly from DuckDB, and write results back where people
already look. There's no download step and no copy to keep in sync.

```sql
SELECT * FROM 'gdrive://Finance/2026/actuals.parquet';                              -- files
SELECT * FROM read_csv('https://docs.google.com/spreadsheets/d/1x2y3z/edit');       -- a Sheet, by its address
SELECT name, modified_by, web_url FROM read_gdrive_files('gdrive://Finance/**');    -- Drive's metadata
COPY results TO 'gdrive://Finance/forecast.parquet' (FORMAT parquet);               -- and back
COPY results TO 'gdrive://Finance/sales' (FORMAT sheet_range, RANGE 'Forecast!A1');  -- or into a Sheet's tab
```

| | read | write |
|---|---|---|
| **Files** in My Drive and Shared Drives | `read_parquet`, `read_csv`, `read_json`, `glob`, `ATTACH`, DuckLake: anything on a `gdrive://` path or a Drive address | `COPY … TO 'gdrive://…'`, streamed |
| **Sheets and Docs** | the same readers: exported as CSV / text; `read_sheet_range` for any tab or A1 range, typed | `COPY … (FORMAT sheet_range, RANGE 'Tab!A1')`, in place |
| **Drive's metadata** | `read_gdrive_files`, `search_gdrive`, `gdrive_versions`, `gdrive_changes`, `gdrive_permissions`, `gdrive_drives` | — |

One install, one secret -- `CREATE SECRET (TYPE gdrive);` reuses your `gcloud`
login. Built for DuckDB **v1.5.6** and **2.0**, by
[DataZoo](https://data-zoo.de), and the sibling of
[duckdb-sharepoint](https://github.com/DataZooDE/duckdb-sharepoint) for
Microsoft 365.

> **🤖 Built for AI pair-programming.** The repository ships a
> **[Claude Code plugin](#claude-code-plugin)**: four skills that teach Claude
> Code this extension's SQL API, from `CREATE SECRET` to change feeds and
> sharing audits. Install with `/plugin marketplace add DataZooDE/duckdb-gdrive`
> → `/plugin install duckdb-gdrive@duckdb-gdrive`.

## Contents

- [Quick start](#quick-start) · [Claude Code plugin](#claude-code-plugin) · [Examples](#examples)
- [Files: `gdrive://`](#files-gdrive) · [Sheets and Docs](#sheets-and-docs) · [Writing](#writing) · [DuckLake on Drive](#ducklake-on-drive)
- [Drive's metadata as tables](#drives-metadata-as-tables): files, search, versions, changes, permissions
- [Installation](#installation) · [Authentication](#authentication) · [Settings](#settings) · [Diagnostics](#diagnostics)
- [Performance and quota](#performance-and-quota) · [Limitations](#limitations) · [When you should NOT use this](#when-you-should-not-use-this)
- [Network and privacy](#network-and-privacy) · [How it works](#how-it-works) · [Building and testing](#building-and-testing)
- [Support and feedback](#support-and-feedback) · [License](#license) · [Citation](#citation)

## Quick start

If you already use `gcloud`, the login is the whole setup -- ask for the Drive
scope, which gcloud's default does not include:

```bash
gcloud auth application-default login \
  --scopes=openid,https://www.googleapis.com/auth/drive
```

```sql
INSTALL gdrive FROM community;
LOAD gdrive;

CREATE SECRET (TYPE gdrive);

-- A Parquet file in My Drive
SELECT count(*) FROM 'gdrive://Finance/2026/actuals.parquet';

-- Or paste an address straight from the browser: a file, or a Sheet
SELECT * FROM read_csv('https://drive.google.com/file/d/1a2b3c4d5e/view?usp=sharing');

-- What is in a folder, with who changed it last
SELECT name, size, modified, modified_by FROM read_gdrive_files('gdrive://Finance/2026');
```

For a server, CI job or anything unattended, name a service-account key
instead -- and bind the secret to a Shared Drive (`SELECT name, id FROM
gdrive_drives()` lists them):

```sql
CREATE SECRET gdrive (
    TYPE gdrive,
    PROVIDER service_account,
    KEY_FILE '/etc/creds/sa.json',
    DRIVE_ID '0ABcDeFgHiJkLmNoPQ',
    DRIVE_SCOPE 'https://www.googleapis.com/auth/drive.readonly'
);
```

[Authentication](#authentication) covers every provider.

## Claude Code plugin

The repository ships a [Claude Code](https://claude.com/claude-code) plugin
with four skills that document this extension's SQL API, so Claude Code can
write queries against Drive from a plain request such as *"list everything
under Finance that anyone with the link can open, and write the list to an
Audit folder"*.

### Install

In any Claude Code session (the plugin is documentation only; DuckDB still
needs `INSTALL gdrive FROM community`):

```
/plugin marketplace add DataZooDE/duckdb-gdrive
/plugin install duckdb-gdrive@duckdb-gdrive
```

Restart Claude Code and the skills are active. You don't call them yourself:
Claude loads the matching skill when a request is about Google Drive, Sheets
or Shared Drives in DuckDB. `/plugin` lists what is installed. To get the
skills of a newer release, run `/plugin marketplace update duckdb-gdrive`.

### Skills

| Skill | Covers |
|---|---|
| `gdrive-setup` | INSTALL/LOAD, `CREATE SECRET` for every provider (gcloud ADC, service account, workload identity, OAuth, token), `DRIVE_ID` vs `ROOT_FOLDER_ID`, `DRIVE_SCOPE`, settings, and what each sign-in or permission error means |
| `gdrive-files` | the `gdrive://` filesystem and globs, the `id:` form, browser addresses, Sheets and Docs, COPY/ATTACH/DuckLake, `write_blob` and friends, the concurrency limits |
| `gdrive-sheets` | `read_sheet_range` and `COPY … (FORMAT sheet_range)`: tabs and A1 ranges, `OVERWRITE` / `APPEND`, typed values, atomicity, the scope and API-enablement errors |
| `gdrive-metadata` | `read_gdrive_files`, `search_gdrive`, `gdrive_drives`, `gdrive_versions` and `?version=`, `gdrive_changes` and the mirror pattern, `gdrive_permissions` |

The skills live in [`plugins/duckdb-gdrive/`](plugins/duckdb-gdrive/). To use
a local checkout instead of the marketplace, start Claude Code with
`claude --plugin-dir ./plugins/duckdb-gdrive`.

## Examples

Each example is one SQL file. Each reads Drive live and writes its result
back to Drive.

| example | from Drive | with | back to Drive |
|---|---|---|---|
| [**Sheet forecast**](examples/sheet-forecast/) | a Google Sheet of monthly sales | [anofox_forecast](https://github.com/DataZooDE/anofox-forecast): AutoETS, 12 months ahead per product | a `Forecast` tab in the same Sheet |
| [**Sharing audit**](examples/sharing-audit/) | every permission under a folder | `gdrive_permissions` joined with `read_gdrive_files` | link-shared, external and stale-but-shared reports |

The forecast, end to end:

```sql
CREATE TABLE sales AS                                                                    -- live from the Sheet
SELECT product, month::DATE AS month, revenue FROM read_sheet_range('https://docs.google.com/spreadsheets/d/1x2y3z/edit', 'sales');
CREATE TABLE forecast AS                                                                 -- anofox_forecast
SELECT product, ds AS month, yhat AS forecast, yhat_lower AS low, yhat_upper AS high
FROM ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo');
COPY forecast TO 'https://docs.google.com/spreadsheets/d/1x2y3z/edit'                  -- back, as a Forecast tab
    (FORMAT sheet_range, RANGE 'Forecast!A1', OVERWRITE);
```

## Files: `gdrive://`

The extension registers a `gdrive://` filesystem. DuckDB sends all file access
through its virtual filesystem, so everything built on it accepts the scheme:
`read_parquet`, `read_csv`, `read_json`, `COPY`, `glob`, `ATTACH` of a DuckDB
database file, and table formats such as DuckLake with their `DATA_PATH` on
Drive.

```sql
-- By path, relative to the secret's root (My Drive, a Shared Drive, or a folder).
SELECT count(*) FROM 'gdrive://Finance/2026/actuals.parquet';

-- By file id: no resolution at all.
SELECT * FROM read_csv('gdrive://id:1a2b3c4d5e');

-- Glob: * ? [a-z] [!x] ** and {a,b} alternation, as in duckdb-sharepoint.
SELECT * FROM read_parquet('gdrive://exports/**/part-[0-9]*.parquet');

-- Write a file. It is streamed in bounded memory and appears when complete.
COPY (SELECT * FROM t) TO 'gdrive://reports/out.parquet' (FORMAT parquet);

-- A DuckDB database file stored in Drive, read-only.
ATTACH 'gdrive://warehouse/sales.duckdb' AS sales (READ_ONLY);

-- Byte-exact round trip for any content, with no CSV or Parquet in between.
SELECT write_blob('gdrive://notes/readme.md', encode('# Title'));
SELECT remove_file('gdrive://notes/readme.md');   -- true, then false
```

| Form | Cost | Use when |
|---|---|---|
| `gdrive://Finance/2026/actuals.parquet` | one API call **per path segment** | writing queries by hand |
| `gdrive://id:1a2b3c...` | zero resolution calls | generated or stored queries |

**Drive has no path addressing.** A path is walked segment by segment, so
`a/b/c.parquet` costs three lookups before a single byte is read. Results are
cached per secret and per drive, so ten files in one folder cost one folder
lookup rather than ten -- but if you are generating queries, prefer the `id:`
form.

**Duplicate names are an error, deliberately.** Drive lets two files share a
name in one folder, so a path is not a unique identifier. Rather than pick one
-- which would make your results depend on Drive's internal ordering -- the
extension fails and names both file ids so you can use the `id:` form. A glob
returns such files as `gdrive://id:` paths.

**Browser addresses.** A file's or folder's address as the browser shows it,
or a *Copy link*, works wherever a path does. Every Drive address names its
file by id, so it costs no resolution:

```sql
SELECT * FROM read_csv('https://drive.google.com/file/d/1a2b3c4d5e/view?usp=sharing');
SELECT * FROM read_csv('https://docs.google.com/spreadsheets/d/1x2y3z/edit#gid=0');
SELECT * FROM read_gdrive_files('https://drive.google.com/drive/folders/1f2g3h');
COPY t TO 'https://drive.google.com/file/d/1a2b3c4d5e/view' (FORMAT csv);   -- overwrites that file
```

These are handled by this extension even when `httpfs` is loaded, which would
otherwise fetch them without credentials -- so an address in a shared SQL
script reads, writes and removes with the credentials of whoever runs it.
`drive.google.com/file/d/…`, `open?id=…`, `uc?id=…`,
`docs.google.com/{spreadsheets,document,presentation}/d/…` all work for
reading, and a `version=<revision id>` in the address's query reads that
version. A folder address (`drive.google.com/drive/folders/<id>`) works in the
metadata functions (`read_gdrive_files`, `gdrive_permissions`,
`gdrive_changes`), not in `read_csv` or `glob`; nothing may follow its id.
`web_url` from `read_gdrive_files` is such an address and reads back.

Writes need the file's own address, `drive.google.com/file/d/<id>/view` (or
`gdrive://id:<id>`): `COPY` stages its output under the address's last
segment, which only in that form is not the id. A docs.google.com editor
address -- a native Sheet or Doc -- is refused, as is writing over any folder
or native Google file: none has bytes to replace.

`write_blob`, `remove_file`, `move_file` and `file_size` don't depend on the
scheme, so they work for any filesystem DuckDB can reach (`s3://`, `gs://`,
local paths). They are shared with duckdb-sharepoint, so both extensions can
be loaded into one database.

> **Google Drive, not Google Cloud Storage.** If your data is in a `gs://`
> bucket you want the `gcs` extension or `httpfs`. This is for files living in
> Drive -- the Sheets, CSV exports and Parquet drops that people actually keep
> in shared folders.

## Sheets and Docs

A Sheet or Doc has no byte stream, so it is served through Drive's export
rather than a normal download: Sheets as `text/csv` (the **first tab**), Docs
as `text/plain` (`SET gdrive_docs_export_mime='text/markdown'` to change
that).

```sql
SELECT * FROM read_csv('gdrive://Finance/Budget');   -- a real Sheet
```

Exports cannot be ranged, so the whole file is fetched **once per open** and
served from a buffer for the rest of that read. It is **not** cached between
queries: querying the same Sheet twice costs two exports. That matters for
quota -- a dashboard polling a Sheet pays the full export every time.
Materialise it into a table if you are going to query it repeatedly.

### A tab or a range of a Sheet: `read_sheet_range` and `FORMAT sheet_range`

> New in v2026.11.13: not in the community build of v2026.11.08 or earlier.

The export above sees the first tab only, as text. To read any tab, or a range
of one, and to **write into a Sheet** -- a forecast into a `Forecast` tab,
results where people already look -- go through the Sheets API instead:

```sql
SELECT * FROM read_sheet_range('gdrive://Finance/sales', 'Actuals!A1:F');      -- A..F, to the last row
SELECT * FROM read_sheet_range('gdrive://Finance/sales', 'Actuals');           -- the whole tab
SELECT * FROM read_sheet_range('gdrive://Finance/sales', '''Q3 plan''!B2:D20', header := false);

COPY forecast TO 'gdrive://Finance/sales' (FORMAT sheet_range, RANGE 'Forecast!A1', OVERWRITE);
COPY new_rows TO 'gdrive://Finance/sales' (FORMAT sheet_range, RANGE 'Log!A1', APPEND);
```

The path is any path a reader takes -- `gdrive://…`, `gdrive://id:<id>`, or the
Sheet's browser address. The range is A1 notation with the **tab named**:
`Tab!A1:F200`, `Tab!A1:F` (open rows), `Tab!A:F`, `Tab!2:5`, a bare `Tab`; a
tab with spaces or quotes is quoted (`'Q3 plan'!A1`, `'It''s'!A1`).

**Reading.** The first row is the header (`header := false` names the columns
`column1`, `column2`, …); `all_varchar := true` reads everything as text.
Formula cells read as their results. Columns are typed as
duckdb-sharepoint's `read_excel_range` types them: only numbers → `BIGINT` or
`DOUBLE`, cells formatted as dates / times → `DATE`, `TIMESTAMP`, `TIME`,
booleans → `BOOLEAN`, anything mixed → `VARCHAR`; an error cell (`#DIV/0!`) is
NULL in a typed column. Trailing empty rows are not returned. One
`spreadsheets.get` per call.

**Writing** changes the Sheet **in place**: it keeps its id, its link, its
sharing and its other tabs, and only the cells of the range change.

| option | |
|---|---|
| `RANGE 'Tab!A1'` | required: the tab, and the top-left cell of the block. A **missing tab is created**. With explicit bounds (`'Tab!A1:F'`) the block must fit them. |
| `HEADER true\|false` | write the column names as the first row (default `true`) |
| *(default)* | write the block from the anchor; nothing outside it is cleared |
| `OVERWRITE` | first clear the anchor's extent -- from the anchor down and right to the edge of the tab, or within the explicit bounds -- then write: a shorter result leaves no stale rows |
| `APPEND` | add the rows below the data already at the anchor (`values.append`); the header is written only when the tab is new or `HEADER true` is given |

- Values are written **typed** and as data, never as formulas: numbers and
  booleans natively, `DATE` / `TIMESTAMP` / `TIME` as Sheets dates with a date
  format (so they read back as dates), text as text -- `'=1+1'` stays the
  string `=1+1`. `NULL` is an empty cell. `DECIMAL` and `HUGEINT` go through
  `DOUBLE` (about 15 significant digits); `LIST`, `STRUCT` and `MAP` columns
  are refused -- convert them first (`to_json(col)`).
- Only cell **values** are touched: formats inside the range, and formulas
  and everything else outside it, stay as they are.
- A write that fits one request (about 2 MB of cell data -- tens of thousands
  of cells) is **one atomic `batchUpdate`**: the new tab, a larger grid, the
  `OVERWRITE` clear and the cells land together or not at all. A larger one is
  split into several requests and is **not atomic**: if one fails, the error
  says the Sheet was partially written and to re-run with `OVERWRITE`. Rows are
  buffered until the query has finished, so a query that fails part-way writes
  nothing.
- A Sheet holds at most 10 million cells across its tabs; a larger write is
  refused before anything is sent.
- The target must already be a native Google Sheet: a CSV or Excel file in
  Drive is refused (write a file with `FORMAT csv` instead), and a plain
  `COPY … (FORMAT csv)` onto a Sheet is still refused -- it would destroy it.
- Writing needs the full `drive` scope (`DRIVE_SCOPE
  'https://www.googleapis.com/auth/drive'`); `drive.readonly` reads a Sheet
  but its writes fail with an insufficient-scope error that says so. The
  Sheets API must be enabled in the Google Cloud project your credential bills
  to -- if it is not, the error names the project and the command:
  `gcloud services enable sheets.googleapis.com --project <project>`.
- Costs one `spreadsheets.get` (the tab list) plus one `batchUpdate` per
  ~2 MB; `gdrive_stats()` counts them as `sheets_get`, `sheets_update` and
  `sheets_append`.

## Writing

```sql
COPY (SELECT * FROM t) TO 'gdrive://reports/out.parquet' (FORMAT parquet);
```

Writes are **sequential only** and **streamed**: a file being written holds at
most one upload chunk in memory (`gdrive_upload_chunk_bytes`, 16 MiB). A file
that never outgrows it is uploaded in one request on close; a larger one opens
a Drive resumable upload session and sends each full chunk as it fills, so a
multi-GB `COPY` needs ~one chunk of RAM, not the file's size (a 373 MiB `COPY`
peaks at ~100 MiB RSS for the whole process). Drive publishes the file only
when the final chunk lands on close: until then nothing appears in the folder
and an overwritten file keeps its old content, and a write that fails
mid-stream -- or a query that errors part-way through a `COPY` -- cancels the
session and leaves nothing behind. (If the final chunk is sent and every
answer to it is lost, a new file is looked up by its reserved id -- see below
-- so the write either succeeds or fails saying nothing was written.) Once the session
is open, chunks carry no token, so a long upload cannot fail on token expiry.
Drive cannot write at a byte offset, so a positional write raises an error
rather than silently corrupting the file.

Names are **case-sensitive**, as Drive stores them: `report.csv` and
`Report.csv` are two files, and writing one never replaces the other. A write
never replaces a folder or a native Doc/Sheet (it would destroy its content),
and a file in the trash is gone -- by path and by `id:`.

`DELETE` moves to **trash**, not permanent deletion -- `SET
gdrive_permanent_delete=true` to opt in. Renames are **not atomic**; DuckDB's
storage manager assumes they are, so do not put a writable DuckDB database
file on `gdrive://`.

Every write is **safe to retry**, and is retried. Creates -- files *and*
folders -- carry an id reserved with `files.generateIds` (one call for a whole
new subtree), so a retry after a dropped connection gets `409 fileIdInUse`
instead of a second object of the same name -- which, given that duplicate
names are a hard error here, would make the path unusable. An overwrite puts
the same bytes on the same id again; a move or a trash sets absolute state.
If Drive cannot be reached on any attempt the error says so and says that
re-running is safe; a create that cannot reserve an id is refused before
anything is written.

**Consistency, stated plainly.** A scan is **not** isolated from a concurrent
overwrite. DuckDB captures a file's size when it opens it, and reads are served
from a block cache keyed by the revision that was current at open. If somebody
replaces the file *while* a query is reading it, that query can mix blocks from
two revisions. Drive offers no way to pin a download to a revision, so this is
not something the extension can fix -- only report. A table format (DuckLake,
Iceberg) avoids it by writing new files rather than mutating old ones.

## DuckLake on Drive

Tested rather than assumed (`test/sql/gdrive_ducklake.test.template`, live
against real Drive):

```sql
INSTALL ducklake; LOAD ducklake;

-- Catalog stays local; only the DATA lives on Drive.
ATTACH 'ducklake:metadata.db' AS lake (DATA_PATH 'gdrive://lakehouse/');
USE lake;

CREATE TABLE t AS SELECT * FROM read_parquet('...');
INSERT INTO t SELECT ...;         -- appends as new Parquet on Drive
DELETE FROM t WHERE grp = 3;      -- writes delete-files alongside
SELECT * FROM t AT (VERSION => 1);-- time travel still reads the old files
```

Verified: write, cold read-back with an exact checksum in a fresh process, a
second transaction, deletes, and time travel. **Keep the catalog off Drive**:
`ducklake:` metadata wants atomic renames and Drive has none. And be blunt
about the fit: this makes Drive a workable *storage layer* for an
occasionally-queried lakehouse. It does not make Drive fast -- see
[Performance and quota](#performance-and-quota). DuckLake's data files are
never rewritten in place, which is what `gdrive_immutable_prefixes` is for.

## Drive's metadata as tables

> New in v2026.11.08: not in the community build of v2026.10.18 or earlier.

Every function takes a `gdrive://` path (the scheme may be left out), an `id:`
path, or a browser address -- naming a file, not one of its versions. A
literal path that names nothing is an error; only a glob may match nothing.
Table functions take no subqueries, so a value from one query goes to the
next through a variable: `SET VARIABLE p = (...)`, then `getvariable('p')`.

### What's in a folder: `read_gdrive_files`

```sql
SELECT name, path, size, modified, modified_by, owner_email, web_url
FROM read_gdrive_files('gdrive://Finance/**/*.xlsx');
```

One row per file a path, glob or folder names (a folder means the files
directly in it): `path`, `name`, `size`, `created`, `modified`, `modified_by`
and `owner` (and their `_email`), `mime_type`, `id`, `web_url`, `version` (the
revision id), `md5`, `description`, `starred`, `shared`, `parent_id`, and the
file's custom `properties` as a MAP. A glob costs exactly the listings `glob()`
makes, with no call per file. Native Docs and Sheets have no `size`, `md5` or
`version` of their own (NULL): their bytes exist only as an export.

Then the file's **lock** -- Drive's content restriction, set with *Lock* in
the Drive UI (*File information → Lock*) or `contentRestrictions` in the API:

> New in v2026.11.13 (not in v2026.11.08 or earlier).

| Column | Type | |
|---|---|---|
| `locked` | BOOLEAN | the file is read-only for everyone until unlocked |
| `locked_by`, `locked_by_email` | VARCHAR | who locked it. Drive hides the address from a service account, so `locked_by_email` is NULL there |
| `locked_at` | TIMESTAMP | when (UTC) |
| `lock_reason` | VARCHAR | the reason given, NULL when none |
| `copy_requires_writer_permission` | BOOLEAN | readers and commenters may not download, print or copy it |

They come in the same listing, so they cost nothing extra. A write to a locked
file -- `COPY`, `write_blob` -- is refused by Drive ("would violate a content
restriction") and leaves the file as it was. These columns were appended after
`properties`, so a `SELECT *` keeps its earlier positions.

```sql
SELECT path, locked_by, locked_at, lock_reason
FROM read_gdrive_files('gdrive://Finance/**') WHERE locked;
```

### Search: `search_gdrive`

```sql
SELECT name, path, modified, modified_by FROM search_gdrive('quarterly forecast');
SELECT name, path FROM search_gdrive('', mime_type := 'application/vnd.google-apps.spreadsheet', max_results := 50);
SET VARIABLE plan = (SELECT path FROM search_gdrive('plan 2026') LIMIT 1);
SELECT * FROM read_csv(getvariable('plan'));
```

Drive's full-text search over names and contents, across **everything the
secret's identity can see** -- its Shared Drive when the secret binds
`DRIVE_ID`, but `ROOT_FOLDER_ID` does not narrow it. Folders are hits too. A
hit's `path` is `gdrive://id:<id>`, so it reads straight back; the columns are
`read_gdrive_files`'. `max_results` (default 500) caps the rows; `mime_type`
filters exactly (`application/vnd.google-apps.spreadsheet`, `…document`,
`…folder`, `text/csv`, …). Drive indexes new content with a delay.

### Shared Drives: `gdrive_drives`

```sql
SELECT name, id FROM gdrive_drives();
SELECT name, id FROM gdrive_drives(search := 'Finance');
```

The Shared Drives the identity is a member of; `id` is what `DRIVE_ID` takes.

### Earlier versions: `gdrive_versions` and `?version=`

Drive keeps a file's revisions, and `?version=<revision id>` reads one of them
with any reader. Earlier versions are read-only:

```sql
SELECT version, current, modified, modified_by, size, path
FROM gdrive_versions('gdrive://Finance/actuals.parquet');
SELECT * FROM read_parquet('gdrive://Finance/actuals.parquet?version=0B9fS7-FFsZTlL285U1lpZ0');
```

Newest first; `path` reads that version. Only uploaded files can be read at a
version: native Docs and Sheets list their revisions but Drive keeps no
downloadable content for them. Drive prunes old revisions of uploaded files
unless they are marked `keep_forever`. An in-place update (`write_blob`, or
`COPY … (USE_TMP_FILE false)`) adds a revision; `COPY`'s default tmp-file
staging replaces the file with a new one, which starts a new history.

### What changed since last time: `gdrive_changes`

`gdrive_changes` reads Drive's changes feed for a folder: one row per file or
folder created or changed under it (`_change = 'upsert'`; a folder renamed or
moved in brings its whole subtree) or trashed or deleted (`'delete'`, by
`id`), then one `'checkpoint'` row whose `token` is what to pass next time.
The token is yours to keep, e.g. in a table:

```sql
CREATE TABLE sync AS SELECT token FROM gdrive_changes('gdrive://Finance', token := 'latest');
-- later:
SET VARIABLE t = (SELECT token FROM sync);
SELECT _change, path, modified_by FROM gdrive_changes('gdrive://Finance', token := getvariable('t'));
```

Without a token it lists everything under the folder once. Drive's feed is per
drive, not per folder: it reports every change the identity can see, and each
is placed by walking its parents (one `files.get` per folder not seen yet). A
file or folder moved **out** looks exactly like one that was never in, so by
default neither is reported; `outside := true` reports both as path-less
`delete`s (a folder with its subtree) -- what a mirror needs, at the cost of a
row and a walk for every change elsewhere in the drive. A permanent deletion
cannot be placed at all and is always reported -- after one `files.get` on its
id, which turns it into an `upsert` if Drive still shows the file under the
folder. Cost: one `files_get` per removed entry in the window, and, with
`outside := true` only, one per change that arrives without parents (such as a
file shared with you). A path that would not read
back (a name shared by siblings, a `/` in a name) is a `gdrive://id:` path.
An expired token says so; start over without one. A very old token replays
everything since, which on a busy drive is a long, sequential walk. The feed
trails a write by seconds to minutes.

**Keeping a mirror.** A table of a folder's files, kept current from the
change feed, with the token stored beside it. Here in DuckLake, so earlier
states stay queryable:

```sql
-- Once (the mirror holds files; folders only shape their paths):
INSTALL ducklake; LOAD ducklake;
ATTACH 'ducklake:mirror.ducklake' AS lake (DATA_PATH 'mirror/');
-- The token first, then the listing: a change in between shows up twice, not never.
CREATE TABLE lake.sync AS
    SELECT token FROM gdrive_changes('gdrive://Finance', token := 'latest');
CREATE TABLE lake.files AS
    SELECT id, path, name, size, modified, modified_by FROM read_gdrive_files('gdrive://Finance/**');

-- Each run (e.g. from cron):
LOAD ducklake;
ATTACH 'ducklake:mirror.ducklake' AS lake (DATA_PATH 'mirror/');
SET VARIABLE t = (SELECT token FROM lake.sync);
-- outside := true: a file or folder moved out of Finance must leave the mirror.
CREATE TEMP TABLE c AS
    SELECT * FROM gdrive_changes('gdrive://Finance', token := getvariable('t'), outside := true);
DELETE FROM lake.files WHERE id IN (SELECT id FROM c WHERE _change <> 'checkpoint');
INSERT INTO lake.files
    SELECT id, path, name, size, modified, modified_by FROM c WHERE _change = 'upsert' AND NOT is_folder;
UPDATE lake.sync SET token = (SELECT token FROM c WHERE _change = 'checkpoint');
```

Running it again is harmless: each run applies what changed since the last.
A delete for an id the feed cannot place is confirmed against Drive before it
is reported, but Drive's feed itself can lag, so a mirror that must never lose
a file should also rebuild from a fresh listing now and then.
The harness checks exactly this -- a mirror that follows the feed ends up equal
to a fresh listing, across deletes, new files, updates, a folder renamed and a
folder moved out (`e2e/tests/test_changes.py`).

### Who can see it: `gdrive_permissions`

`gdrive_permissions` lists, for a file or folder or the files **and folders** a
glob matches, one row per permission -- one `permissions.list` call per item,
so a tree of thousands takes minutes: the `role`, how it was granted (`direct`, `inherited`,
or general access by `link`), the user, group or domain, and for general
access its `link_scope` (`anonymous` = anyone with the link, `organization` = a
domain) and whether search can find it (`discoverable`). For audits such as
"what is shared with anyone":

```sql
SELECT path, role, link_url, discoverable
FROM gdrive_permissions('gdrive://Finance/**')
WHERE granted_by = 'link' AND link_scope = 'anonymous';
```

As Drive reports them: a grant on a file that does not exceed one inherited
from its folder is not recorded by Drive, so it reads as `inherited` -- which
is what Drive enforces. `inherited_from` is set for Shared Drive items only.

## Installation

```sql
INSTALL gdrive FROM community;
LOAD gdrive;
```

The community build is the default. A tag-fresh mirror is also published to
the erpl.io bucket on every release. That binary is unsigned, so start DuckDB
with `-unsigned` (or set `allow_unsigned_extensions`):

```sql
-- duckdb -unsigned
INSTALL gdrive FROM 'http://get.erpl.io';
LOAD gdrive;
```

## Authentication

| Provider | Use |
|---|---|
| `credential_chain` | **default.** Application Default Credentials -- your `gcloud` login, or `GOOGLE_APPLICATION_CREDENTIALS` |
| `service_account` | unattended: servers, CI, scheduled jobs |
| `authorization_code` | interactive browser consent, with your own OAuth client |
| `config` | you already hold an access/refresh token |

### `credential_chain` -- the default

Takes no arguments. On first use it looks for a credential in the order
Google's own tooling uses:

1. `gdrive_adc_file`, if that DuckDB setting is set
2. `$GOOGLE_APPLICATION_CREDENTIALS`
3. `$CLOUDSDK_CONFIG/application_default_credentials.json`
4. `~/.config/gcloud/application_default_credentials.json`
   (`%APPDATA%\gcloud\…` on Windows)

Three document kinds are accepted: an `authorized_user` file (what `gcloud
auth application-default login` writes) refreshes access tokens as it goes, a
`service_account` key file is used to mint them via RFC 7523, and an
`external_account` file (workload identity federation, below) exchanges a
platform-issued token for one.

> **Request the Drive scope at login.** gcloud's default ADC scope is
> `cloud-platform`, which does **not** include Drive, so a plain `gcloud auth
> application-default login` produces a credential that fails every read with
> a 403. Pass `--scopes=openid,https://www.googleapis.com/auth/drive`. The
> extension detects this specific case and says so rather than reporting a
> generic permission error.
>
> Note also that plain `gcloud auth login` is not enough -- it configures the
> CLI, not Application Default Credentials.

> **On a Google Workspace account that login may be refused outright**, with
> a browser page reading *"This app is blocked -- This app tried to access
> sensitive info in your Google Account."* Nothing is misconfigured, and the
> extension is not involved: the refusal happens at Google's consent screen,
> before any token exists.
>
> The cause is that `gcloud auth application-default login` requests the
> scope using **gcloud's own built-in OAuth client**
> (`764086051850-…apps.googleusercontent.com`), Drive is a *restricted*
> scope, and a Workspace domain decides for itself which OAuth clients may
> request restricted scopes. A domain that has not explicitly trusted the
> Cloud SDK client blocks it. Personal Google accounts are unaffected.
>
> Three ways out, in rough order of least work:
>
> 1. **Use a service-account key** and point `GOOGLE_APPLICATION_CREDENTIALS`
>    or `gdrive_adc_file` at it. This is still `credential_chain` -- the
>    `service_account` arm -- so no SQL changes. Service accounts mint tokens
>    via RFC 7523 with no consent screen, so restricted scopes never arise.
>    Remember a service account can only write inside a **Shared Drive**.
> 2. **Have a Workspace admin trust the Cloud SDK client**: admin.google.com
>    → Security → Access and data control → API controls → *Manage app
>    access* → Configure new app → search that client id → **Trusted**.
>    *Limited* is not enough: it permits only unrestricted scopes. Searching
>    by client id returns it under the name **Google Auth Library** (verified,
>    Google-owned), not "gcloud" or "Cloud SDK" -- that is the right entry.
>    Scope it to the org unit containing the user who needs Drive.
> 3. **Bring your own OAuth client** (type *Desktop app*, as under
>    `authorization_code` below) and pass `gcloud auth
>    application-default login --client-id-file=…`. That writes the same
>    `authorized_user` document and needs no domain-wide decision.
>
> One more trap while you are there: each run of `application-default login`
> **replaces** the stored credential rather than adding to it. If you need
> Drive *and* ordinary Cloud API access from the same ADC file, ask for both
> in a single login -- `--scopes=openid,https://www.googleapis.com/auth/cloud-platform,https://www.googleapis.com/auth/drive`.

#### Workload identity federation (`external_account`)

For a workload that should not hold a downloaded key -- a Kubernetes pod, for
example -- `credential_chain` also accepts an `external_account` credential
configuration (what `gcloud iam workload-identity-pools create-cred-config`
writes). It contains no secret. On each token mint the extension reads the
subject token from disk, exchanges it at STS (`token_url`) for a federated
token, then impersonates the service account named by
`service_account_impersonation_url` with the Drive scope. Point
`credential_chain` at the file like any other ADC document:

```sql
SET gdrive_adc_file = '/etc/gdrive/wif-config.json';
CREATE SECRET wif_drive (TYPE gdrive);
```

or set `GOOGLE_APPLICATION_CREDENTIALS` to it. A minimal configuration:

```json
{
  "type": "external_account",
  "audience": "//iam.googleapis.com/projects/PROJECT_NUMBER/locations/global/workloadIdentityPools/POOL/providers/PROVIDER",
  "subject_token_type": "urn:ietf:params:oauth:token-type:jwt",
  "token_url": "https://sts.googleapis.com/v1/token",
  "service_account_impersonation_url": "https://iamcredentials.googleapis.com/v1/projects/-/serviceAccounts/SA_EMAIL:generateAccessToken",
  "credential_source": { "file": "/var/run/secrets/gcp/token" }
}
```

What is supported, exactly:

- **Only a file-sourced subject token** (`credential_source.file`), such as a
  projected Kubernetes service-account token. The file is re-read on every
  mint, so the platform's hourly rotation is picked up. It must hold the bare
  token (`format.type` `text`, the default); trailing newlines are trimmed.
  A `format.type` of `json` (token in `subject_token_field_name`) is not
  supported and is refused, naming `format`, before any request is made.
- **`url`-sourced tokens (Azure, GCE/OIDC metadata), AWS-sourced tokens and
  `executable`-sourced tokens are not supported.** A configuration without
  `credential_source.file` is refused when the secret is first used, with a
  message naming `credential_source`.
- **`service_account_impersonation_url` is required.** A federated token alone
  cannot carry the Drive scope; without it the mint fails, after the STS hop,
  with a message saying so. The federated principal needs `roles/iam.workloadIdentityUser`
  on that service account.
- `audience`, `token_url` and `subject_token_type` are required and named if
  missing. The impersonated token is requested for one hour; workforce pools
  (`workforce_pool_user_project`) and
  `service_account_impersonation.token_lifetime_seconds` are not read.
- As with any service account, writes only succeed inside a **Shared Drive**.

### `config` -- a token you already have

For a token obtained elsewhere. `ACCESS_TOKEN` alone is used as-is and never
refreshed -- once Drive rejects it, every query fails with an authentication
error until the secret is recreated. `REFRESH_TOKEN` with `CLIENT_ID` and
`CLIENT_SECRET` is refreshed as needed; given beside an `ACCESS_TOKEN`, the
pasted token is used first and the refresh token takes over when Drive rejects
it.

Every refreshable secret (all providers except a bare `ACCESS_TOKEN`) keeps
working for as long as the process does. A scan or an `ATTACH`ed database that
outlives its access token (about an hour) gets a fresh one, and a `401` is
answered with one new token and one retry. Tokens are cached and minted under
a per-credential lock, so handles and threads share one token rather than each
minting their own. A refresh that fails (a revoked refresh token) reports the
error and is not retried against Google's token endpoint for 30 seconds.
`gdrive_stats()` counts both (`auth_refresh`, `token_request`).

This is the right provider when consent happened on a *different* machine --
do the browser flow on your laptop, then copy the refresh token to the server.
For consent on *this* machine, use `authorization_code` below, which does the
same thing without the copying.

### `authorization_code` -- browser consent

```sql
CREATE SECRET my_drive (
    TYPE gdrive, PROVIDER authorization_code,
    CLIENT_ID '…apps.googleusercontent.com',
    CLIENT_SECRET '…',
    DRIVE_SCOPE 'https://www.googleapis.com/auth/drive'
);
```

Creating the secret stores configuration only. On first use a browser opens,
you consent, and a loopback redirect hands the code back. **The refresh token
is stored in the secret**, so that is the last time you see a consent screen --
not once per hour.

**Use `CREATE PERSISTENT SECRET` if you want that to survive a restart.** A
plain `CREATE SECRET` is temporary: the refresh token lives in the secret for
the session and is gone when the process exits, so the next process prompts
for consent again. A persistent secret is written by DuckDB to
`~/.duckdb/stored_secrets`, which means a Drive refresh token sits on your
disk in DuckDB's own store -- the right trade for a workstation, the wrong one
for a shared host. This extension itself never writes credentials to disk.

> **You bring your own OAuth client**, and that is a deliberate trade. Create
> one of type *Desktop app* in the Google Cloud console (enable the Drive API
> first). We could instead compile a DataZoo-owned client id into the
> extension and make this a zero-argument statement -- that is how the
> `gsheets` extension manages `CREATE SECRET (TYPE gsheet);` -- but it would
> put our name on your consent screen, cap usage at 100 test users until
> Google verification completes, and Drive scopes are "restricted", so full
> verification means a third-party security assessment. If you want zero
> setup, use `credential_chain` above: it reuses the `gcloud` login you
> already have.

**Make it a *Desktop app* client, not a *Web application* one.** For desktop
clients Google accepts a loopback redirect on any port, so there is nothing to
register and `REDIRECT_PORT` can be changed freely. A web client checks the
redirect URI exactly, and you would have to register

```
http://localhost:8020
```

-- note the bare path. Get either detail wrong and Google answers
`Error 400: redirect_uri_mismatch` before the consent screen appears.

On a machine with no display the flow fails immediately and says so, rather
than opening nothing and timing out. `REDIRECT_PORT` (default `8020`) changes
the loopback port if that one is taken; on a web client it has to match what
you registered.

`authorization_code` secrets cannot be used for an `ATTACH`ed database file:
refreshing them writes to the session, which the database-level opener
DuckDB uses for attached files does not have. Such an ATTACH fails with an
error that says so.

**A service account needs a Shared Drive to write.** Service accounts have no
personal Drive storage quota, so they cannot own files in a My Drive -- Google
returns `403 storageQuotaExceeded` on upload. Share a folder with the service
account for read-only use, or use a Shared Drive for read/write.

### Binding a root

| Option | When |
|---|---|
| `DRIVE_ID` | the root is a **Shared Drive** (its id starts `0A`; `gdrive_drives()` lists them) |
| `ROOT_FOLDER_ID` | the root is an **ordinary folder**, shared with your identity |

These are not interchangeable. Drive's API scopes a Shared Drive query with
`corpora=drive`+`driveId`, and sending those for a plain folder returns
`404 "Shared drive not found"` -- which reads like a permissions problem and is
nothing of the sort.

> **`DRIVE_SCOPE`, not `SCOPE`.** `SCOPE` is a reserved DuckDB clause meaning
> *"which paths may use this secret"*; DuckDB's parser consumes it before an
> extension ever sees it. Writing `SCOPE 'https://www.googleapis.com/auth/drive'`
> silently restricts the secret to paths beginning with that URL -- which no
> `gdrive://` path does -- so the secret matches nothing and every query fails
> with "a gdrive secret exists, but none apply", which names this mistake. The
> OAuth scope goes in `DRIVE_SCOPE`.

Omitting `DRIVE_SCOPE` requests the narrowest scope that works,
`drive.readonly`. **How much that protects you depends on the provider**, and
the difference is not ours to fix:

| Provider | Effect of the default |
|---|---|
| `service_account` | **Enforced.** The scope is a claim in the signed assertion, so Google refuses a write with *"Request had insufficient authentication scopes"* -- tested. |
| `config` | **Advisory.** A refresh-token exchange returns an access token carrying the scopes granted at *consent* time; asking for a narrower one does not narrow it. If the token was consented for full Drive access, it keeps it. |
| `credential_chain` | **Depends on what was found.** A `service_account` or `external_account` document behaves as the first row (for `external_account` the scope is requested at impersonation); an `authorized_user` document behaves as the second, so the scope is fixed by the `--scopes` you passed to `gcloud`. |
| `authorization_code` | **Enforced at consent.** `DRIVE_SCOPE` is what the consent screen asks for, so this is the one interactive case where the default genuinely narrows access. |

So with `config` -- and with a `gcloud` credential -- restrict at consent time;
`DRIVE_SCOPE` cannot claw back access the refresh token already carries.

Tokens live in memory or in DuckDB secrets, are never written to disk by this
extension, and never appear in an error message.

## Settings

Every setting the extension registers, and what it is for. `scripts/verify_readme.py`
checks this table in **both** directions -- nothing here that does not exist,
and nothing registered that is not here.

| Setting | Default | What it does |
|---|---|---|
| `gdrive_docs_export_mime` | `text/plain` | Export format for a Google **Doc**: `text/plain`, `text/markdown` (keeps structure), `text/html`, `application/pdf`, `application/rtf`, the docx, ODT and EPUB types, or `application/zip` (zipped HTML); anything else is refused when set. Text exports lose Drive's byte-order mark. Sheets are always `text/csv`. |
| `gdrive_permanent_delete` | `false` | `false` moves a deleted file to the Drive trash (recoverable); `true` deletes it outright. |
| `gdrive_telemetry_enabled` | `true` | Anonymous usage telemetry shared with the other DataZoo extensions. `false` turns it off for the rest of the session; the environment variable `DATAZOO_DISABLE_TELEMETRY=1` turns it off entirely, including the load event. |
| `gdrive_block_size_bytes` | 16 MiB | Read granularity. Drive charges ~1.3 s per ranged request regardless of size, so larger blocks mean fewer, faster requests -- but a `count(*)` over a Parquet footer then pays for a whole block it does not need. 16 MiB is the compromise; see `docs/benchmark.md` for the sweep. |
| `gdrive_block_cache_bytes` | 256 MiB | Total cap on the shared block cache, across all files and handles. Blocks are keyed by identity + file id + revision, so a file changing on Drive cannot be served stale. |
| `gdrive_immutable_prefixes` | *(empty)* | Comma-separated `gdrive://` prefixes whose files are **never overwritten in place** -- typically a DuckLake/Iceberg `DATA_PATH`. Skips the per-open metadata refresh (~270 ms per file per query) for them. Matching is on whole path segments, so `gdrive://lake` does not cover `gdrive://lakehouse`. **Declaring a prefix whose files change causes stale reads that nothing can detect.** The promise you are making is stronger than "not overwritten": files under it must not be modified, replaced, *or* deleted-and-recreated at the same path while the process runs -- Drive keeps a file's id across an overwrite, returns no ETag, and ignores `If-Match`. Leave empty unless you know the files are immutable. |
| `gdrive_adc_file` | *(empty)* | An Application Default Credentials file for `credential_chain`, overriding `GOOGLE_APPLICATION_CREDENTIALS` and the gcloud location. |
| `gdrive_debug_token_max_age_seconds` | 0 | **Testing only.** Treats access tokens as expiring after at most this many seconds, so a handle outliving its token can be exercised in a test instead of after an hour. `0` uses the token's real lifetime. |
| `gdrive_upload_chunk_bytes` | 16 MiB | Upload chunk size -- and the most memory one file being written holds. Files up to this size upload in one request on close; larger ones stream through a resumable upload session, one chunk at a time, as they are written. Rounded down to a multiple of 256 KiB (Drive's rule for every non-final chunk), at least 256 KiB and at most 1 GiB. `gdrive_stats()` reports `upload_chunks`, `upload_aborts` and the high-water mark `upload_buffer_peak_bytes`. |
| `gdrive_path_cache_entries` | 4096 | Cap on cached `path -> file id` mappings, LRU. `0` is unbounded. Drive has no path addressing, so each dropped mapping costs one `files.list` per segment to rebuild -- cheap, which is why this cache may be evicted and the block cache is bounded separately. |

```sql
SELECT name, value FROM duckdb_settings() WHERE name LIKE 'gdrive%';
```

An `ATTACH`ed database file sees only **global** settings, so a `gdrive_*`
setting made with a plain `SET` does not apply to it -- use `SET GLOBAL`.

## Diagnostics

```sql
CALL gdrive_reset_stats();
SELECT * FROM read_parquet('gdrive://exports/**/*.parquet');
SELECT * FROM gdrive_stats();   -- files_get, files_list, files_media, ..., generate_ids,
                                -- permissions_list, revisions_list, changes_list, retries,
                                -- cache_hits, total
```

`gdrive_stats()` counts the Drive API calls by kind for the process, plus the
path cache's hits and size, `auth_refresh` (401s that triggered a token
refresh, at most one per call), `token_request` (token mint attempts) and
`generate_ids` (the id reservations that make creates retry-safe: one per
created file, or one per new subtree). Reset, run one query, read it: that is
what that query cost. Every call is retried with jittered exponential backoff
on dropped connections, 5xx and rate limits -- writes included, because every
write the extension sends is safe to repeat (see above).

Two environment variables, both **off** unless set. Neither is a DuckDB
setting: they are read once at first use, so a flag cannot flip underneath a
running scan and leave a trace with holes in it.

| Variable | What it does |
|---|---|
| `GDRIVE_TRACE_FILE` | Writes one JSON line per Drive API attempt to that path: start and end time (µs from process start), thread, call kind, HTTP status, response size, whether the attempt paid for a new TLS connection, and the retry number. This is what shows *overlap* -- `gdrive_stats()` counts calls, but only a timeline shows that a cold DuckLake read spends its first 1.7 s on six sequential `files.list` calls before requesting a byte. |
| `GDRIVE_TRACE_RANGES` | Older, media-only: prints `off=/len=/ms=` per ranged read to stderr. Kept because it is quoted in `docs/benchmark.md`; prefer `GDRIVE_TRACE_FILE`. |

When unset the cost is one already-initialised `static const bool` and a
predictable branch -- the clock is never read. A span carries no URL, header,
body or file id, so a trace can be shared without leaking a credential or the
contents of a Drive.

```bash
GDRIVE_TRACE_FILE=/tmp/scan.jsonl duckdb -c "SELECT count(*) FROM 'gdrive://a/b.parquet'"
```

Storage-quota and rate-limit errors are deliberately **different messages**.
Conflating them is the specific failure this guards against: a service account
writing outside a Shared Drive gets a storage error that is not retryable and
not about rate, and telling someone to "retry later" would waste their day.

## Performance and quota

Reads are ranged, so a Parquet scan fetches footers and column chunks rather
than whole files.

> **The target is met, narrowly.** The goal is a cold columnar scan within
> **3×** the same file on object storage. Measured 2026-08-01 on an 87 MB
> Parquet, all legs in one session, 9 repeats: local **0.11 s**,
> S3 **0.97 s**, GCS **1.35 s**, `gdrive://` **3.93 s** -- **2.91×** GCS.
> With `gdrive_block_size_bytes` at 128 MiB, **2.60 s -- 1.93×**, at the cost of
> ~0.8 s on footer-only queries like `count(*)`. That is why the knob exists
> and why 16 MiB remains the default.
>
> **Do not read 2.91× as an improvement.** An earlier run measured 3.05× and
> nothing in the extension changed between them: this benchmark addresses its
> fixture through the `gdrive://id:` form, which skips path resolution
> entirely. The denominator moved. Two object stores in the same city differ
> by 39% here -- quoted against S3 the identical run is **4.05×** and fails the
> gate. We report GCS because that is what the gate was written against, not
> because it is the kinder number.
>
> **A lakehouse workload is worse than a single scan.** DuckLake operations run
> 4–10× an object store, and the *cheapest* operation has the worst ratio: a
> time-travel read is 10.4×, because it reads almost no data and is nearly pure
> metadata. The full scan, moving the most bytes, is the best at 4.0×. Budget
> by how many lookups a query does, not by how much data it moves.
>
> Also: a cold scan is ~36× a local file, and downloading first and querying
> locally is **6.14 s**, slower than querying in place.

Drive enforces per-user API quotas. A query that looks unremarkable against S3
can hit them, so the extension caches path resolution aggressively and reports
a quota error explicitly rather than as a generic failure. See
`docs/benchmark.md` and `docs/paper/` for the measurements.

## Limitations

Early, but read *and* write are real and verified against a live Google Drive
on every change. Not production-ready; what follows is stated here rather than
discovered later.

- **`DRIVE_SCOPE` does not restrict a refresh token.** With `PROVIDER config`
  and a `REFRESH_TOKEN`, the scope was fixed at consent; a secret declared
  `drive.readonly` will still write -- verified. Grant only the read-only scope
  at consent, or use a service-account key whose assertion names the scope.
- **Concurrent writers create duplicates -- of files *and* of directories.**
  Drive has no atomic create-if-absent, so every create is check-then-act.
  Measured 2026-08-02: three parallel `COPY … TO 'gdrive://out/shared/dir/fN.csv'`
  produced **three folders named `out`**, after which the whole subtree was
  unaddressable by path; two writers to the same path made two files in **6 of
  6 trials**. The extension adopts a folder another writer created if it sees
  one, which narrows the window; nothing in the Drive API can close it. Give
  each writer its own prefix (a table format does), and recover a poisoned
  path by deleting one copy via `gdrive://id:<fileId>`.
- **A failed `move_file` onto an existing target can leave two files.** The
  move is done before the replaced file is deleted, deliberately -- the reverse
  risks losing data. Both steps are retried (each is safe to repeat), but if
  the delete still fails -- refused, or Drive unreachable on every attempt --
  the call raises with both files in place.
- **Rate-limit errors are not covered by live tests.** Storage-quota errors are
  provoked against the real API; rate limits (403/429) only against captured
  Drive response bodies, because exhausting real quota degrades Drive for
  everyone on that account.
- **`ATTACH` of a DuckDB database file is read-only** and uses global settings
  only. Writable attachment is impossible: Drive has neither atomic renames nor
  byte-offset writes.
- **Native Sheets and Docs are re-exported per open**, and a Sheet *exported*
  through `read_csv` gives its first tab only -- `read_sheet_range` reads any
  tab. Earlier versions of native files cannot be read.
- **A `sheet_range` write larger than one request is not atomic.** About 2 MB
  of cell data per `batchUpdate`; a larger write is split, and a failure
  part-way leaves the range partially written (re-run with `OVERWRITE`). An
  `APPEND` is never retried automatically, because a repeated append appends
  twice. `FORMAT sheet_range` writes into an existing Sheet; it does not create
  a new spreadsheet, and there is no keyed upsert (yet).
- **Search and the changes feed lag** writes, by seconds to minutes.

## When you should NOT use this

Being direct, because the alternatives are often better:

- **A workstation?** Use Google Drive for desktop and ordinary local paths.
  Zero code, faster, and correct. This extension exists for headless servers,
  where a logged-in desktop client is not an option.
- **A FUSE mount (`rclone mount`)?** Presents Drive as a local path and needs
  no DuckDB extension at all. If you already run one and it performs
  acceptably, keep it. The trade is operational rather than technical: a mount
  to provision and monitor on every host, and behaviour under concurrency and
  partial reads that is outside your control. This extension needs nothing on
  the host but the extension itself -- and gives you Drive's metadata
  (versions, changes, permissions) as tables, which no mount does.
- **A large, hot dataset queried constantly?** Copy it to object storage. A
  ranged read from Drive costs ~1.3 s regardless of how few bytes it asks for,
  against ~1 ms for S3/GCS, and Drive enforces per-user API quotas that object
  storage does not. This is the right tool when Drive is the *system of record*
  and you want to stop maintaining a copy -- not when you want a fast warehouse.
- **Only Sheets, nothing else in Drive?** The `gsheets` community extension
  reads and writes cell ranges through the Sheets API too, with a one-line
  secret. This one does the same for Sheets in Drive (`read_sheet_range`,
  `FORMAT sheet_range`) next to the files and Drive's metadata, over one
  secret.

## Network and privacy

Your data goes only between DuckDB and Google:

| what | where | opt out |
|---|---|---|
| Files, metadata, exports, uploads | the Drive API (`www.googleapis.com`) | — |
| Sheet ranges (`read_sheet_range`, `FORMAT sheet_range`) | the Sheets API (`sheets.googleapis.com`) | — |
| Sign-in and token refresh | Google's token endpoint (`oauth2.googleapis.com`); for workload identity also `sts.googleapis.com` and `iamcredentials.googleapis.com` | — |
| Browser consent (`authorization_code`) | your browser and a loopback redirect on `localhost` | — |
| Telemetry | one anonymous `extension_loaded` event to PostHog EU (`eu.i.posthog.com`): the extension's name and version, DuckDB's version, OS and architecture, whether it runs in CI or a container, and a salted SHA-256 hash of the machine id (never the id itself) | `SET gdrive_telemetry_enabled = false`, or `DATAZOO_DISABLE_TELEMETRY=1` to stop the load event too |

Access tokens are held in memory only and never appear in an error message.
Refresh tokens are stored only in a `PERSISTENT` secret, in DuckDB's secret
directory (see [Authentication](#authentication)). Telemetry never includes
your data, paths, file names or SQL.

## How it works

Google Drive has **no path addressing**: resolving `a/b/c.parquet` costs one
`files.list` per segment, which is why the extension carries a path → id cache
(keyed by the credential's fingerprint, so a secret recreated under the same
name with different credentials starts afresh). SharePoint, by contrast,
addresses items by path natively -- the sibling extension needs no such cache.

- **Reads** are ranged and served from a shared, single-flight block cache
  keyed by file id and revision.
- **Writes** stream through a resumable upload session, reserving a file id
  first so a retry cannot create a duplicate.
- **Metadata functions** resolve their path through the same glob and cache,
  asking the listings for the extra fields they return.

The design, and the Drive behaviours found along the way, are in
[docs/hld.md](docs/hld.md); the measurements in
[docs/benchmark.md](docs/benchmark.md).

## Building and testing

```sh
git clone --recursive https://github.com/DataZooDE/duckdb-gdrive
cd duckdb-gdrive
GEN=ninja make      # extension + Catch2 binary
make test           # Catch2 pure-logic + SQL tests (live tests skip without credentials)
make test_live      # SQL suites against a real Drive; fails loudly without credentials
make e2e            # live end-to-end harness (Python + uv)
make check_pin check_credentials check_cstdint check_stamp smoke_loadable
```

There are no mocks: everything Drive-facing is proven against a real Google
Drive -- there is no fake server and no recorded-HTTP layer. The fixtures live
in a folder in a user's My Drive, shared with a CI service account; writes run
as that delegated user, because a service account cannot own files outside a
Shared Drive. (`DRIVE_ID`, a Shared Drive root, has a live test that runs only
once a CI Shared Drive is configured.) See
`scripts/setup_ci_drive.sh` and [CLAUDE.md](CLAUDE.md) for the workflow, the
recorded decisions and the pitfalls worth not rediscovering.

## Support and feedback

- Design notes and the Drive behaviours found along the way: [docs/hld.md](docs/hld.md)
- Issues: [GitHub Issues](https://github.com/DataZooDE/duckdb-gdrive/issues)

If `gdrive` misbehaves, please [open an issue](https://github.com/DataZooDE/duckdb-gdrive/issues).
That includes an auth flow that will not complete, a file that will not read,
a glob that misses, or a change feed that skips something. Drive behaves
differently with account type, Shared Drive setup and quota in ways we can't
reproduce here, so a report with your setup is the fastest path to a fix.
Errors end with that link.

If it saved you time, a star on the repo helps other people find it.

The first time you load the extension in an interactive terminal each day, a
small banner says the same. It never prints when output is piped, in notebooks
or in CI. Silence it with `SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

## License

MIT © 2026 DataZoo GmbH. See [LICENSE](LICENSE).

## Citation

If you use this extension in research, please cite:

```bibtex
@software{duckdb_gdrive,
  title  = {duckdb-gdrive: Google Drive in SQL for DuckDB},
  author = {Joachim Rosskopf and DataZoo GmbH},
  year   = {2026},
  url    = {https://github.com/DataZooDE/duckdb-gdrive}
}
```
