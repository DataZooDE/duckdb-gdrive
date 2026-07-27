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
`read_csv`, `COPY`, `glob`, and table formats such as DuckLake.

`ATTACH` is the exception, in both directions. Attaching a **DuckLake** whose
`DATA_PATH` is on Drive works and is tested. Attaching a **DuckDB database
file** on `gdrive://` does not — DuckDB's storage manager needs atomic
renames and byte-offset writes, and Drive offers neither, so the attempt
fails early rather than corrupting anything.

## DuckLake on Drive

That last one is the interesting case, so it is **tested rather than assumed**
(`test/sql/gdrive_ducklake.test.template`, live against real Drive):

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

Verified: write, cold read-back with an exact checksum in a fresh process,
a second transaction, deletes, and time travel — with the Parquet data and
delete files visible through `glob('gdrive://lakehouse/**')`.

**Keep the catalog off Drive.** `ducklake:` metadata wants atomic renames and
Drive has none, which is the same reason a DuckDB database file must not live
on `gdrive://`. Point `DATA_PATH` at Drive and leave the catalog on a local
disk or a real database.

Worth being blunt about the fit: this makes Drive a workable *storage layer*
for an occasionally-queried lakehouse. It does not make Drive fast — see
*Performance and quota*.

> **Google Drive, not Google Cloud Storage.** If your data is in a `gs://`
> bucket you want the `gcs` extension or `httpfs`. This is for files living in
> Drive — the Sheets, CSV exports and Parquet drops that people actually keep
> in shared folders.

## Status

Early, but read *and* write are real. **Verified against a live Google Drive
account** (102 assertions across four live suites, plus 860 pure-logic ones):

- reads by file id and by path, multi-segment resolution, UTF-8 and spaced
  names, ranged Parquet reads, globbing, listing past Drive's 100-item page
  default, native Sheet export, and the duplicate-name error;
- writes (`COPY … TO`), overwrite, move, trash-vs-permanent delete, and the
  storage-quota error a service account gets outside a Shared Drive;
- hive-partitioned reads, and **byte-for-byte identity** between a local read
  and the same file over `gdrive://`;
- **DuckLake** end to end, including deletes and time travel.

**Not yet verified: the performance target.** The `gdrive://` and local
benchmark legs are measured, but the 3×-GCS gate has no GCS denominator yet,
so REQ-NF-01 is unproven — see `docs/benchmark.md`, which says so plainly
rather than quoting the half of the result that looks good.

Not production-ready. `docs/implementation-plan.md` lists exactly what is
verified and what is not.

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

`config` covers the interactive case today: obtain a refresh token once (any
OAuth2 client will do — see `scripts/oauth_consent.py` for a working example)
and hand it to the secret, which refreshes access tokens itself.

> **No `authorization_code` provider yet.** An in-process browser consent flow
> is designed (`docs/hld.md` §5) and the machinery exists in the
> `datazoo-oauth2` library, but it is **not registered**, so `PROVIDER
> authorization_code` is an error. It was listed here before it existed; that
> was a documentation bug, and `make verify_readme` now checks this table
> against the providers actually registered.

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

Omitting `DRIVE_SCOPE` requests the narrowest scope that works,
`drive.readonly`. **How much that protects you depends on the provider**, and
the difference is not ours to fix:

| Provider | Effect of the default |
|---|---|
| `service_account` | **Enforced.** The scope is a claim in the signed assertion, so Google refuses a write with *"Request had insufficient authentication scopes"* — tested. |
| `config` | **Advisory.** A refresh-token exchange returns an access token carrying the scopes granted at *consent* time; asking for a narrower one does not narrow it. If the token was consented for full Drive access, it keeps it. |

So with `config`, restrict at consent time — `DRIVE_SCOPE` cannot claw back
access that the refresh token already carries.

Tokens live in memory or in DuckDB secrets, are never written to disk by this
extension, and never appear in an error message.

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

Storage-quota and rate-limit errors are deliberately **different messages**.
Conflating them is the specific failure this guards against: a service account
writing outside a Shared Drive gets a storage error that is not retryable and
not about rate, and telling someone to "retry later" would waste their day.

> **Known coverage gap.** The storage-quota path is asserted against the live
> API. The **rate-limit** path is asserted only against captured 403/429
> response bodies in `test/cpp/test_errors.cpp`, not live — exhausting Drive
> API quota needs a throwaway Google project, and provoking it against a real
> one degrades Drive for everyone on that account. Stating this rather than
> letting the suite imply coverage it does not have.

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
