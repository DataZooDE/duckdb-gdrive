# duckdb-gdrive

A DuckDB extension that registers a `gdrive://` filesystem over **Google
Drive**, so any DuckDB path expression can address a Drive file directly — no
download, no copy step.

If you already use `gcloud`, that is the whole setup:

```bash
gcloud auth application-default login \
  --scopes=openid,https://www.googleapis.com/auth/drive
```

```sql
INSTALL gdrive FROM community;
LOAD gdrive;

CREATE SECRET (TYPE gdrive);

SELECT count(*) FROM 'gdrive://Finance/2026/actuals.parquet';
```

For a server, CI job, or anything unattended, name a key file instead:

```sql
CREATE SECRET gdrive (
    TYPE gdrive,
    PROVIDER service_account,
    KEY_FILE '/etc/creds/sa.json',
    DRIVE_ID '0ABcDeFgHiJkLmNoPQ',
    DRIVE_SCOPE 'https://www.googleapis.com/auth/drive.readonly'
);
```

Because DuckDB dispatches all file access through its virtual filesystem,
anything built on that layer inherits the scheme for free — `read_parquet`,
`read_csv`, `COPY`, `glob`, and table formats such as DuckLake.

`ATTACH` is the exception, in both directions. Attaching a **DuckLake** whose
`DATA_PATH` is on Drive works and is tested. Attaching a **DuckDB database
file** on `gdrive://` does not, and the reason is worth stating precisely
because it is not the one you would guess: DuckDB opens database files
through a path that never consults the virtual filesystem, so `gdrive://` is
normalised to `gdrive:/` and handed to the LOCAL filesystem, which reports
`No such file or directory`. This extension is never called at all.

That it fails is fine — Drive has neither atomic renames nor byte-offset
writes, so a database file there could not work regardless — but it fails
before reaching us, which is why the guard rails in this filesystem
(`Truncate`, `Trim`, positional `Write`) are unreachable from SQL and
therefore untested. Stated rather than implied; see `docs/benchmark.md` and
the plan's coverage table.

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
account** (209 assertions across six live suites, 23 e2e, plus 937 pure-logic
ones):

- reads by file id and by path, multi-segment resolution, UTF-8 and spaced
  names, ranged Parquet reads, globbing, listing past Drive's 100-item page
  default, native Sheet export, and the duplicate-name error;
- writes (`COPY … TO`), overwrite, move, trash-vs-permanent delete, and the
  storage-quota error a service account gets outside a Shared Drive;
- hive-partitioned reads, and **byte-for-byte identity** between a local read
  and the same file over `gdrive://`;
- **DuckLake** end to end, including deletes and time travel.

**The performance target is met, narrowly.** A cold columnar scan measures
**2.91×** the same file on GCS against a 3× goal, and **1.93×** with 128 MiB
blocks. Read that with two caveats. It passed at 3.05× a release earlier and
nothing in the extension changed to move it — the object-store denominator did,
and quoted against S3 (0.97 s) the same run is 4.05× and would fail. And a
*lakehouse* workload is 4–10× an object store, not 3×, because Drive's penalty
tracks how many things you look up rather than how many bytes you move. See
`docs/benchmark.md` and `docs/paper/`.

Not production-ready. The *Known limitations* section below lists what is
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
| `credential_chain` | **default.** Application Default Credentials — your `gcloud` login, or `GOOGLE_APPLICATION_CREDENTIALS` |
| `service_account` | unattended: servers, CI, scheduled jobs |
| `authorization_code` | interactive browser consent, with your own OAuth client |
| `config` | you already hold an access/refresh token |

### `credential_chain` — the default

Takes no arguments. On first use it looks for a credential in the order
Google's own tooling uses:

1. `gdrive_adc_file`, if that DuckDB setting is set
2. `$GOOGLE_APPLICATION_CREDENTIALS`
3. `$CLOUDSDK_CONFIG/application_default_credentials.json`
4. `~/.config/gcloud/application_default_credentials.json`
   (`%APPDATA%\gcloud\…` on Windows)

Both document kinds are accepted: an `authorized_user` file (what `gcloud auth
application-default login` writes) refreshes access tokens as it goes, and a
`service_account` key file is used to mint them via RFC 7523.

> **Request the Drive scope at login.** gcloud's default ADC scope is
> `cloud-platform`, which does **not** include Drive, so a plain `gcloud auth
> application-default login` produces a credential that fails every read with
> a 403. Pass `--scopes=openid,https://www.googleapis.com/auth/drive`. The
> extension detects this specific case and says so rather than reporting a
> generic permission error.
>
> Note also that plain `gcloud auth login` is not enough — it configures the
> CLI, not Application Default Credentials.

> **On a Google Workspace account that login may be refused outright**, with
> a browser page reading *"This app is blocked — This app tried to access
> sensitive info in your Google Account."* Nothing is misconfigured, and the
> extension is not involved: the refusal happens at Google's consent screen,
> before any token exists.
>
> The cause is that `gcloud auth application-default login` requests the
> scope using **gcloud's own built-in OAuth client**
> (`764086051850-…apps.googleusercontent.com`), Drive is a *restricted*
> scope, and a Workspace domain decides for itself which OAuth clients may
> request restricted scopes. A domain that has not explicitly trusted the
> Cloud SDK client blocks it. Personal Google accounts are unaffected, which
> is why this is easy to miss until it happens to you.
>
> Three ways out, in rough order of least work:
>
> 1. **Use a service-account key** and point `GOOGLE_APPLICATION_CREDENTIALS`
>    or `gdrive_adc_file` at it. This is still `credential_chain` — the
>    `service_account` arm — so no SQL changes. Service accounts mint tokens
>    via RFC 7523 with no consent screen, so restricted scopes never arise.
>    Remember a service account can only write inside a **Shared Drive**.
> 2. **Have a Workspace admin trust the Cloud SDK client**: admin.google.com
>    → Security → Access and data control → API controls → *Manage app
>    access* → Configure new app → search that client id → **Trusted**.
>    *Limited* is not enough: it permits only unrestricted scopes. Searching
>    by client id returns it under the name **Google Auth Library** (verified,
>    Google-owned), not "gcloud" or "Cloud SDK" — that is the right entry.
>    Scope it to the org unit containing the user who needs Drive.
> 3. **Bring your own OAuth client** (type *Desktop app*, as under
>    `authorization_code` below) and pass `gcloud auth
>    application-default login --client-id-file=…`. That writes the same
>    `authorized_user` document and needs no domain-wide decision.
>
> One more trap while you are there: each run of `application-default login`
> **replaces** the stored credential rather than adding to it. If you need
> Drive *and* ordinary Cloud API access from the same ADC file, ask for both
> in a single login — `--scopes=openid,https://www.googleapis.com/auth/cloud-platform,https://www.googleapis.com/auth/drive`.

Workload identity federation (`external_account`) is not supported; the
extension names it explicitly rather than failing obscurely.

### `config` — a token you already have

For a token obtained elsewhere. `ACCESS_TOKEN` alone is used as-is and never
refreshed; `REFRESH_TOKEN` with `CLIENT_ID` and `CLIENT_SECRET` is refreshed
as needed.

This is the right provider when consent happened on a *different* machine —
do the browser flow on your laptop, then copy the refresh token to the server.
For consent on *this* machine, use `authorization_code` below, which does the
same thing without the copying.

### `authorization_code` — browser consent

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
is stored in the secret**, so that is the last time you see a consent screen —
not once per hour.

**Use `CREATE PERSISTENT SECRET` if you want that to survive a restart.** A
plain `CREATE SECRET` is temporary: the refresh token lives in the secret for
the session and is gone when the process exits, so the next process prompts
for consent again. A persistent secret is written by DuckDB to
`~/.duckdb/stored_secrets`, which means a Drive refresh token sits on your
disk in DuckDB's own store — the right trade for a workstation, the wrong one
for a shared host. This extension itself never writes credentials to disk.

> **You bring your own OAuth client**, and that is a deliberate trade. Create
> one of type *Desktop app* in the Google Cloud console (enable the Drive API
> first). We could instead compile a DataZoo-owned client id into the
> extension and make this a zero-argument statement — that is how the
> `gsheets` extension manages `CREATE SECRET (TYPE gsheet);` — but it would
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

— note the bare path. Get either detail wrong and Google answers
`Error 400: redirect_uri_mismatch` before the consent screen appears.

On a machine with no display the flow fails immediately and says so, rather
than opening nothing and timing out. `REDIRECT_PORT` (default `8020`) changes
the loopback port if that one is taken; on a web client it has to match what
you registered.

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
| `credential_chain` | **Depends on what was found.** A `service_account` document behaves as the first row; an `authorized_user` document behaves as the second, so the scope is fixed by the `--scopes` you passed to `gcloud`. |
| `authorization_code` | **Enforced at consent.** `DRIVE_SCOPE` is what the consent screen asks for, so this is the one interactive case where the default genuinely narrows access. |

So with `config` — and with a `gcloud` credential — restrict at consent time;
`DRIVE_SCOPE` cannot claw back access the refresh token already carries.

Tokens live in memory or in DuckDB secrets, are never written to disk by this
extension, and never appear in an error message.

## Native Google formats

A Sheet or Doc has no byte stream, so it is served through Drive's export
rather than a normal download: Sheets as `text/csv`, Docs as `text/plain`
(`SET gdrive_docs_export_mime='text/markdown'` to change that).

Exports cannot be ranged, so the whole file is fetched **once per open** and
served from a buffer for the rest of that read. It is **not** cached between
queries: querying the same Sheet twice costs two exports. That matters for
quota — a dashboard polling a Sheet pays the full export every time, and
unlike a Parquet read there is no footer-only shortcut. Materialise it into a
table if you are going to query it repeatedly.

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

### Consistency, stated plainly

A scan is **not** isolated from a concurrent overwrite. DuckDB captures a
file's size when it opens it, and reads are served from a block cache keyed by
the revision that was current at open. If somebody replaces the file *while*
a query is reading it, that query can mix blocks from two revisions.

Drive offers no way to pin a download to a revision, so this is not something
the extension can fix — only report. In practice it matters for a file being
rewritten under a live query, which is exactly the pattern a table format
(DuckLake, Iceberg) avoids by writing new files rather than mutating old ones.

Creates are **idempotent**: the extension reserves a file id
(`files.generateIds`) before writing, so a retry after a dropped connection
cannot leave you with two files of the same name — which, given that duplicate
names are a hard error here, would otherwise make the path unusable.

Writing Parquet to Drive produces an opaque binary file. If you were hoping to
*see* your data in Drive afterwards, you will be disappointed — export to CSV
instead.

## Known limitations

Stated here rather than discovered later.

**A failed write may or may not have happened.** Drive's simple upload is not
idempotent: if it commits a `files.create` and the response is then lost to a
network failure, retrying creates a *second* file with the same name. Because
duplicate names in one folder are a hard error here (see *Addressing*), a
blind retry can poison a path permanently — you would write once and read
"ambiguous" forever.

So transport failures are retried only for idempotent methods (`GET`,
`DELETE`). A `COPY` that fails mid-flight returns an error saying the change
may or may not have been applied, and does **not** retry. Check the path
before re-running it. The real fix is Drive's resumable upload protocol, whose
session URI makes a retry genuinely idempotent; that is not implemented yet.

**`DRIVE_SCOPE` does not restrict a refresh token.** With `PROVIDER config`
and a `REFRESH_TOKEN`, the scope was fixed when consent was granted; Google
returns a token carrying those scopes and refreshing cannot narrow them. So a
secret declared with `DRIVE_SCOPE '…/auth/drive.readonly'` will still write —
verified. Treat `DRIVE_SCOPE` as *what to ask for at consent time*, not as a
guard rail on an existing credential. If you need a genuinely read-only
credential, grant only the read-only scope during consent, or use a
service-account key whose assertion names the scope on every mint.

**Concurrent writers create duplicates — of files *and* of directories.**
Drive has no atomic create-if-absent, so every create is check-then-act: the
writers all look, all see nothing, and all create. This applies to a shared
output *directory* just as much as to a file, and the writers report success.
Measured 2026-08-02: three parallel `COPY … TO
'gdrive://out/shared/dir/fN.csv'` produced **three folders named `out`**, after
which the whole subtree was unaddressable by path.

The extension adopts a folder another writer created if it sees one, which
narrows the window, but it cannot close it — nothing in the Drive API can.
**Do not point concurrent writers at one directory tree.** Give each writer its
own top-level prefix, or serialise the creation of the shared parents before
fanning out. A table format does the former by construction, which is why
DuckLake is unaffected.

**Two concurrent writers to the SAME path will poison it.** Drive has no
atomic create-if-absent, so the write path is check-then-act: both writers list
the destination, both see nothing, both create. The result is two files with
one name — and because duplicate names are a hard error here, *every*
subsequent read, glob and delete of that path fails with "ambiguous" until you
remove one by file id. Measured 2026-08-02: **6 of 6 trials**, with both
`write_blob` and `COPY … TO`. This is not a rare race; two writers that overlap
at all will hit it.

There is no fix available at the storage layer — `files.generateIds` makes a
*retry* idempotent, not two independent creates. Serialise writers to a path
yourself, or give each writer a distinct name (which is what a table format
does, and why DuckLake is unaffected). Recovering a poisoned path means
listing the duplicates and deleting one via `gdrive://id:<fileId>`.

**A failed `move_file` onto an existing target can leave two files.** The move
is done before the replaced file is deleted, deliberately — the reverse order
risks losing data if the move fails. If the delete then fails, the destination
name has two entries. The call raises, so this is loud rather than silent, but
the path needs the same id-based cleanup.

**Rate-limit errors are not covered by live tests.** Storage-quota errors are
provoked against the real API; rate limits (403/429) are asserted only against
captured Drive response bodies, because exhausting real quota needs a
throwaway Google project and doing it to a real one degrades Drive for
everyone on that account. The classification logic is tested; the live path
through it is not.

**`ATTACH` of a DuckDB database file does not work** — see *Addressing*. Only
DuckLake `DATA_PATH` works.

**Native Sheets and Docs are re-exported per open.** Two queries against the
same Sheet cost two exports. Materialise into a table if you query it
repeatedly.

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
- **A large, hot dataset queried constantly?** Copy it to object storage. A
  ranged read from Drive costs ~1.3 s regardless of how few bytes it asks for,
  against ~1 ms for S3/GCS, and Drive enforces per-user API quotas that object
  storage does not. This is the right tool when Drive is
  the *system of record* and you want to stop maintaining a copy — not when you
  want a fast warehouse.

## Performance and quota

Reads are ranged, so a Parquet scan fetches footers and column chunks rather
than whole files.

> **The target is met, narrowly.** The goal is a cold columnar scan within
> **3×** the same file on object storage. Measured 2026-08-01 on an 87 MB
> Parquet, all legs in one session, 9 repeats: local **0.11 s**,
> S3 **0.97 s**, GCS **1.35 s**, `gdrive://` **3.93 s** — **2.91×** GCS.
> With `gdrive_block_size_bytes` at 128 MiB, **2.60 s — 1.93×**, at the cost of
> ~0.8 s on footer-only queries like `count(*)`. That is why the knob exists
> and why 16 MiB remains the default.
>
> **Do not read 2.91× as an improvement.** An earlier run measured 3.05× and
> nothing in the extension changed between them: this benchmark addresses its
> fixture through the `gdrive://id:` form, which skips path resolution
> entirely. The denominator moved. Two object stores in the same city differ
> by 39% here — quoted against S3 the identical run is **4.05×** and fails the
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

## Settings

Every setting the extension registers, and what it is for. `scripts/verify_readme.py`
checks this table in **both** directions — nothing here that does not exist,
and nothing registered that is not here.

| Setting | Default | What it does |
|---|---|---|
| `gdrive_docs_export_mime` | `text/plain` | Export format for a Google **Doc**. `text/markdown` keeps structure. Sheets are always `text/csv`. |
| `gdrive_permanent_delete` | `false` | `false` moves a deleted file to the Drive trash (recoverable); `true` deletes it outright. |
| `gdrive_block_size_bytes` | 16 MiB | Read granularity. Drive charges ~1.3 s per ranged request regardless of size, so larger blocks mean fewer, faster requests — but a `count(*)` over a Parquet footer then pays for a whole block it does not need. 16 MiB is the compromise; see `docs/benchmark.md` for the sweep. |
| `gdrive_block_cache_bytes` | 256 MiB | Total cap on the shared block cache, across all files and handles. Blocks are keyed by identity + file id + revision, so a file changing on Drive cannot be served stale. |
| `gdrive_immutable_prefixes` | *(empty)* | Comma-separated `gdrive://` prefixes whose files are **never overwritten in place** — typically a DuckLake/Iceberg `DATA_PATH`. Skips the per-open metadata refresh (~270 ms per file per query) for them. Matching is on whole path segments, so `gdrive://lake` does not cover `gdrive://lakehouse`. **Declaring a prefix whose files change causes stale reads that nothing can detect.** The promise you are making is stronger than "not overwritten": files under it must not be modified, replaced, *or* deleted-and-recreated at the same path while the process runs — Drive keeps a file's id across an overwrite, returns no ETag, and ignores `If-Match`. Leave empty unless you know the files are immutable. |
| `gdrive_path_cache_entries` | 4096 | Cap on cached `path -> file id` mappings, LRU. `0` is unbounded. Drive has no path addressing, so each dropped mapping costs one `files.list` per segment to rebuild — cheap, which is why this cache may be evicted and the block cache is bounded separately. |

```sql
SELECT name, value FROM duckdb_settings() WHERE name LIKE 'gdrive%';
```

### Diagnostics

Two environment variables, both **off** unless set. Neither is a DuckDB
setting: they are read once at first use, so a flag cannot flip underneath a
running scan and leave a trace with holes in it.

| Variable | What it does |
|---|---|
| `GDRIVE_TRACE_FILE` | Writes one JSON line per Drive API attempt to that path: start and end time (µs from process start), thread, call kind, HTTP status, response size, whether the attempt paid for a new TLS connection, and the retry number. This is what shows *overlap* — `gdrive_stats()` counts calls, but only a timeline shows that a cold DuckLake read spends its first 1.7 s on six sequential `files.list` calls before requesting a byte. |
| `GDRIVE_TRACE_RANGES` | Older, media-only: prints `off=/len=/ms=` per ranged read to stderr. Kept because it is quoted in `docs/benchmark.md`; prefer `GDRIVE_TRACE_FILE`. |

When unset the cost is one already-initialised `static const bool` and a
predictable branch — the clock is never read. A span carries no URL, header,
body or file id, so a trace can be shared without leaking a credential or the
contents of a Drive.

```bash
GDRIVE_TRACE_FILE=/tmp/scan.jsonl duckdb -c "SELECT count(*) FROM 'gdrive://a/b.parquet'"
```

## Development

See `CLAUDE.md` for the build and test workflow, the recorded decisions
(D-1 … D-8) and a list of pitfalls worth not rediscovering.

```bash
GEN=ninja make          # build
make test               # pure-logic + SQL (live tests skip without credentials)
make test_live          # live tests; fails loudly without credentials
```

Integration tests run against **real Google Drive** — there is no fake server
and no recorded-HTTP layer. See `scripts/setup_ci_drive.sh`.

## Licence

MIT. See `LICENSE`.

## Feedback

If `gdrive` misbehaves — an auth flow that will not complete, a file that will not
read, a glob that misses — please [open an issue](https://github.com/DataZooDE/duckdb-gdrive/issues).
Drive behaviour varies with account type, shared-drive setup and quota in ways we cannot
reproduce here, so a report with your setup is the fastest path to a fix. Filesystem
errors end with that link.

If it saved you time, a star on the repo helps other people find it.

The first time you load the extension in an interactive terminal each day, a small
banner says the same thing. It never prints when output is piped, in notebooks, or in
CI. Silence it with `SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

