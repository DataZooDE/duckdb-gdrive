# duckdb-gdrive

A DuckDB extension that registers a `gdrive://` filesystem over **Google
Drive**, so any DuckDB path expression can address a Drive file directly — no
download, no copy step.

```sql
INSTALL gdrive FROM community;
LOAD gdrive;

CREATE SECRET gdrive (
    TYPE gdrive,
    PROVIDER service_account,
    KEY_FILE '/etc/creds/sa.json',
    DRIVE_ID '0ABcDeFgHiJkLmNoPQ',
    DRIVE_SCOPE 'https://www.googleapis.com/auth/drive.readonly'
);

SELECT count(*) FROM 'gdrive://Finance/2026/actuals.parquet';
```

Because DuckDB dispatches all file access through its virtual filesystem,
anything built on that layer inherits the scheme for free — `read_parquet`,
`read_csv`, `COPY`, `glob`, `ATTACH`, and table formats such as DuckLake.

> **Google Drive, not Google Cloud Storage.** If your data is in a `gs://`
> bucket you want the `gcs` extension or `httpfs`. This is for files living in
> Drive — the Sheets, CSV exports and Parquet drops that people actually keep
> in shared folders.

## Status

Early, but the read path is real. **Verified against a live Google Drive
account**: reads by file id and by path, multi-segment resolution, UTF-8 and
spaced names, ranged Parquet reads, globbing, listing past Drive's 100-item
page default, native Sheet export, and the duplicate-name error.

**Not yet verified: the write path** (`COPY … TO`), deletion, moves, and the
performance target. Do not use this in production yet.
See `docs/implementation-plan.md` for exactly what is verified and what is not.

## Addressing

| Form | Cost | Use when |
|---|---|---|
| `gdrive://Finance/2026/actuals.parquet` | one API call **per path segment** | writing queries by hand |
| `gdrive://id:1a2b3c...` | zero resolution calls | generated or stored queries |

Drive has **no path addressing**. A path is walked segment by segment, so
`a/b/c.parquet` costs three lookups before a single byte is read. Results are
cached per secret and per drive, so ten files in one folder cost one folder
lookup rather than ten — but if you are generating queries, prefer the `id:`
form.

**Duplicate names are an error, deliberately.** Drive lets two files share a
name in one folder, so a path is not a unique identifier. Rather than pick one
— which would make your results depend on Drive's internal ordering — the
extension fails and names both file ids so you can use the `id:` form.

## Authentication

| Provider | Use |
|---|---|
| `service_account` | unattended: servers, CI, scheduled jobs |
| `config` | you already hold an access/refresh token |
| `authorization_code` | interactive browser consent |

**A service account needs a Shared Drive to write.** Service accounts have no
personal Drive storage quota, so they cannot own files in a My Drive — Google
returns `403 storageQuotaExceeded` on upload. Share a folder with the service
account for read-only use, or use a Shared Drive for read/write.

### Binding a root

| Option | When |
|---|---|
| `DRIVE_ID` | the root is a **Shared Drive** (its id starts `0A`) |
| `ROOT_FOLDER_ID` | the root is an **ordinary folder**, shared with your identity |

These are not interchangeable. Drive's API scopes a Shared Drive query with
`corpora=drive`+`driveId`, and sending those for a plain folder returns
`404 "Shared drive not found"` — which reads like a permissions problem and is
nothing of the sort.

> **`DRIVE_SCOPE`, not `SCOPE`.** `SCOPE` is a reserved DuckDB clause meaning
> *"which paths may use this secret"*; DuckDB's parser consumes it before an
> extension ever sees it. Writing `SCOPE 'https://www.googleapis.com/auth/drive'`
> silently restricts the secret to paths beginning with that URL — which no
> `gdrive://` path does — so the secret matches nothing and every query fails
> with "no secret configured". The OAuth scope goes in `DRIVE_SCOPE`.

Scopes default to the narrowest that works (`drive.readonly`). Tokens live in
memory or in DuckDB secrets, are never written to disk by this extension, and
never appear in an error message.

## Native Google formats

A Sheet or Doc has no byte stream, so it is served through Drive's export
rather than a normal download: Sheets as `text/csv`, Docs as `text/plain`
(`SET gdrive_docs_export_mime='text/markdown'` to change that). Exports cannot
be ranged, so the whole file is fetched once and cached.

```sql
SELECT * FROM read_csv('gdrive://Finance/Budget');   -- a real Sheet
```

## Writing

```sql
COPY (SELECT * FROM t) TO 'gdrive://reports/out.parquet' (FORMAT parquet);
```

Writes are **sequential only**: bytes buffer locally and upload once on close.
Drive cannot write at a byte offset, so a positional write raises an error
rather than silently corrupting the file. This suits engines that emit whole
immutable files, which is most of them.

`DELETE` moves to **trash**, not permanent deletion — `SET
gdrive_permanent_delete=true` to opt in. Renames are **not atomic**; DuckDB's
storage manager assumes they are, so do not put a DuckDB database file on
`gdrive://`.

Writing Parquet to Drive produces an opaque binary file. If you were hoping to
*see* your data in Drive afterwards, you will be disappointed — export to CSV
instead.

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
  the host but the extension itself.
- **A large, hot dataset queried constantly?** Copy it to object storage. Drive
  round trips are ~150 ms against ~1 ms for S3/GCS, and Drive enforces per-user
  API quotas that object storage does not. This is the right tool when Drive is
  the *system of record* and you want to stop maintaining a copy — not when you
  want a fast warehouse.

## Performance and quota

Expect a bounded, stated penalty rather than parity — the target is a cold
columnar scan within **3×** the same file on object storage. Reads are ranged,
so a Parquet scan fetches footers and column chunks rather than whole files.

Drive enforces per-user API quotas. A query that looks unremarkable against S3
can hit them, so the extension caches path resolution aggressively and reports
a quota error explicitly rather than as a generic failure. `SELECT * FROM
gdrive_stats()` shows the API calls and cache hits for the session.

## Development

See `CLAUDE.md` for the build and test workflow, and
`docs/implementation-plan.md` for the plan and its recorded decisions.

```bash
GEN=ninja make          # build
make test               # pure-logic + SQL (live tests skip without credentials)
make test_live          # live tests; fails loudly without credentials
```

Integration tests run against **real Google Drive** — there is no fake server
and no recorded-HTTP layer. See `scripts/setup_ci_drive.sh`.

## Licence

MIT. See `LICENSE`.
