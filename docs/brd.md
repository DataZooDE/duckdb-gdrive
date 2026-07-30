# BRD — `duckdb-gdrive`: a Google Drive filesystem extension for DuckDB

**Status:** Draft for decision. Nothing is committed.
**Owner:** unassigned
**Date:** 2026-07-26

---

## 1. Summary

DuckDB can read files from local disk, `s3://`, `gs://` (Google *Cloud
Storage*) and plain HTTP. It cannot read from Google Drive, which is an
unrelated product with a different API and access model.

This proposes a DuckDB extension registering a `gdrive://` filesystem, so that
any DuckDB path expression — `read_parquet`, `read_csv`, `COPY`, `glob` — can
address a file in Google Drive directly, with no download or copy step.

*(As shipped, `ATTACH` is the exception: a DuckLake `DATA_PATH` on Drive works,
but DuckDB opens database files through a path that never consults the virtual
filesystem, so `ATTACH 'gdrive://x.duckdb'` is not supported. This proposal
originally listed `ATTACH` without that qualification.)*

---

## 2. Problem

A large amount of real operational data lives in Google Drive: CSV exports,
finance workbooks, Parquet extracts dropped into shared folders, Sheets
maintained by hand. It lives there because Drive is where the people who own
that data already work, and where its sharing and permissions are already
administered.

Querying any of it with DuckDB today requires one of:

- a human downloading the file and pointing DuckDB at a local path;
- a scheduled job copying Drive → object storage;
- mounting Drive as a filesystem on the machine running DuckDB.

The first does not scale and goes stale immediately. The second introduces a
second copy of data the customer did not ask to duplicate, plus a pipeline to
maintain, plus staleness between runs. The third is viable on a workstation
but not on a headless server (see §9).

**The gap: Drive is a system of record for data people want to query, and
DuckDB cannot reach it.**

---

## 3. Scope boundary

This is a **filesystem** extension. It makes Drive-hosted *files* addressable
as DuckDB paths. It is not:

- **a synchroniser.** No change watching, no webhooks, no two-way sync.
- **a Drive management surface.** Sharing, permissions and revisions are not
  exposed as tables. If wanted, that is a separate table-function extension.
- **a way to make binary formats readable in Drive.** Writing Parquet to Drive
  produces opaque binary files that a person cannot open in a browser. Anyone
  hoping to *see* their data in Drive by pointing a columnar engine at it will
  be disappointed; that expectation should be corrected early rather than
  discovered late.

One consequence worth stating positively: because DuckDB dispatches all file
access through its virtual filesystem, **anything built on that layer inherits
the scheme for free** — including table formats such as DuckLake, whose data
files and cleanup routines go through the same interface. That is the leverage
of implementing here rather than in any one consumer.

---

## 4. Users and use cases

| # | User | Use case |
|---|---|---|
| U-1 | Analyst | `SELECT * FROM 'gdrive://Finance/2026/actuals.csv'` — query a file a colleague maintains, without downloading it |
| U-2 | Application embedding DuckDB | Read customer-curated reference data straight from the customer's own Drive, so the application is never serving stale copies |
| U-3 | Data engineer | Read a Parquet extract another team drops into a shared folder, without provisioning object-storage access for that team |
| U-4 | Operator | Ad-hoc investigation over files already shared into a Drive folder, from the DuckDB CLI, on a server |

The unifying property: **Drive is already the system of record for that data,
and its access is already administered there.** This extension is valuable
exactly when that is true, and not otherwise.

---

## 5. Requirements

### Functional

| # | Requirement |
|---|---|
| **REQ-F-01** | Register a DuckDB filesystem claiming a distinct URI scheme, so `gdrive://…` works anywhere DuckDB accepts a path. |
| **REQ-F-02** | Read files by path, including **ranged** reads, so columnar formats fetch footers and column chunks without transferring whole objects. |
| **REQ-F-03** | List and glob a folder, so `gdrive://folder/*.parquet` resolves. |
| **REQ-F-04** | Report file size and last-modified time. |
| **REQ-F-05** | Authenticate via DuckDB's secret manager, supporting a service account (unattended) and OAuth user credentials (interactive), including Shared Drives. |
| **REQ-F-06** | Write files (create + overwrite), so `COPY … TO 'gdrive://…'` works. Lower priority than read — see §8 R-3. |
| **REQ-F-07** | Export native Google formats on read: a Sheet readable as CSV, a Doc as text, so a native file is queryable rather than an error. |
| **REQ-F-08** | Surface Drive failures as actionable DuckDB errors, distinguishing not-found, permission-denied, quota-exceeded and expired-credential from each other. |

### Non-functional

| # | Requirement |
|---|---|
| **REQ-NF-01** | **Latency.** A Drive API round trip costs 0.3–2.1 s depending on the request shape — ~0.3 s for metadata, ~1.3 s for a ranged read of a large file — against ~1 ms for object storage (measured, `docs/benchmark.md`; an earlier "~150 ms" here was an assumption and was wrong by up to 14x). Repeated reads of one file within a query must be served from cache; a cold columnar scan must cost a bounded number of round trips, not one per column chunk. |
| **REQ-NF-02** | **Quota.** Drive enforces per-user API quotas, unlike object storage. A single query must not amplify into hundreds of metadata calls, and hitting a limit must produce a clear error rather than silent throttling. |
| **REQ-NF-03** | **Credential handling.** No credential is written to disk by the extension, logged, or included in an error message. No credential file is ever committed to the repository. |
| **REQ-NF-04** | **Least privilege.** Request the narrowest workable Drive scope; read-only configurations request a read-only scope. |
| **REQ-NF-05** | **Portability.** Linux, macOS (arm64 + x86_64), Windows. |
| **REQ-NF-06** | **Maintenance.** DuckDB extensions are rebuilt per DuckDB release. This is a standing cost for the life of the project, not a one-off. |

### Architectural

| # | Requirement |
|---|---|
| **REQ-A-01** | The provider-agnostic OAuth2 machinery **must be extracted into a shared library, `datazoo-oauth2`**, consumed by this extension rather than copied into it. Vendoring a private copy is explicitly rejected. |
| **REQ-A-02** | **`erpl-web` must be migrated onto that library in the same effort**, and its existing OAuth2 test suites must pass unchanged afterwards. The extraction is not complete while two copies of the code exist — a "shared" library with one consumer and one fork is worse than either, because it carries the coordination cost without the benefit. |
| **REQ-A-03** | The library must contain **no provider-specific code**. Endpoints, scopes, secret type names, extra authorization parameters and provider-specific grant flows belong to the consuming extension. Anything Google- or Microsoft-shaped appearing in the library is a design failure. |
| **REQ-A-04** | The library must have its **own tests, runnable without network access or credentials**, so correctness is not established only through its consumers. |

---

## 6. Success criteria

1. `SELECT count(*) FROM 'gdrive://<folder>/<file>.parquet'` returns the correct
   count against a real Drive account, from a clean install, following only the
   README.
2. A cold 100 MB Parquet scan over Drive completes within **3×** the wall-clock
   of the same file over object storage on the same connection. Not parity — a
   bounded and stated penalty.
3. A native Google Sheet is queryable without the user exporting it first.
4. A credential error, a permission error and a quota error each produce a
   distinct, actionable message.
5. The extension builds and its tests pass on all target platforms in CI.
6. **`datazoo-oauth2` exists as a standalone library with its own passing
   tests, and `erpl-web` consumes it with its OAuth2 suites green and no
   remaining copy of the extracted code** (REQ-A-01/02).

---

## 7. Out of scope

- Change notification, watching or two-way synchronisation.
- Writing native Google formats. Writes produce ordinary binary files; creating
  or updating a real Sheet or Doc is out.
- A SQL surface over Drive metadata (sharing, permissions, revisions).
- Concurrent multi-part uploads.
- Any attempt to make binary file formats human-readable inside Drive.

---

## 8. Risks

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| **R-1** | **Drive has no path addressing.** Resolving `a/b/c.parquet` costs one lookup per path segment, so metadata calls can dominate a query. | High | Per-connection path→fileId cache; a direct-fileId path form that bypasses resolution; measure the amplification factor as an explicit benchmark rather than assuming it. |
| **R-2** | **Quota exhaustion** on a workload that looks unremarkable on object storage. | High | Cache aggressively; surface rate-limit responses explicitly; document the ceiling rather than let users discover it. |
| **R-3** | **Writes are not positional.** Drive cannot write at a byte offset, so DuckDB's positional `Write` cannot be honoured in general. | Medium | Support sequential writes only (buffer locally, upload once on close) and throw a clear error on positional writes rather than silently corrupting. Most writers emit whole files. |
| **R-4** | **Name collisions.** Drive permits two files with the same name in one folder, so a path is not a unique identifier. | Medium | Treat multiple matches as an error naming both file ids. Silently picking one would make query results depend on Drive's internal ordering. |
| **R-5** | **Maintenance treadmill.** Every DuckDB release needs a rebuild and possibly API fixes. | Medium | Accept knowingly. Do not start without a committed owner. |
| **R-6** | **OAuth UX.** Interactive consent from a database session is awkward. | Medium | Service account as the primary path; user OAuth secondary. |
| **R-7** | **Effort underestimated** because the happy path is easy and the edge cases are not. | Medium | Milestone 1 is a throwaway spike whose stated purpose is to measure R-1 and R-2 before committing further. |
| **R-8** | **The extraction destabilises `erpl-web`,** a shipping extension with paying users, for the benefit of a project that has not yet proven itself. | High | Extraction is behaviour-preserving by construction: move code, change no logic, and gate the merge on `erpl-web`'s existing OAuth2 test suites passing unchanged. If M1 kills the Drive project, the extraction still stands on its own merits — it is not wasted work. |
| **R-9** | **Cross-repository coordination.** Two repositories and two release cycles now move together; a breaking change in the library needs both updated. | Medium | Pin the library by commit in each consumer, so neither is forced to upgrade on the other's schedule. Requires a named owner for the library itself, not just for each extension. |

---

## 9. Alternatives considered

| Alternative | Assessment |
|---|---|
| **`rclone` (or similar) FUSE mount** | Presents Drive as a local path; needs no DuckDB work whatsoever. **This is the strongest alternative** and must be benchmarked before any C++ is written. If it performs comparably, this project should not proceed. Its weaknesses are operational: a mount to provision and monitor per host, and behaviour under concurrency and partial reads that is outside our control. |
| **Google Drive desktop sync + local paths** | Zero code, correct answer on a workstation. Rejected for servers: needs a logged-in desktop client and syncs whole folders to local disk. Should be documented as the recommended answer for laptop use. |
| **Scheduled copy Drive → object storage** | Works with existing tooling and is the right answer for large, hot datasets. Rejected as the general answer because it reintroduces staleness and a duplicate copy of customer data — the thing U-2 exists to remove. |
| **Extend the existing `gcs` community extension** | Different product, different API and auth model. Nothing meaningful is shared beyond project scaffolding. |
| **Do nothing** | Viable. The use cases are real but none is currently blocking. |

---

## 10. Existing assets that reduce the work

Authentication was expected to be a significant share of this project. It is
not, because a complete, provider-agnostic OAuth2 client already exists in the
`erpl-web` extension, built for SAP and then generalised to Microsoft Entra.

**Extracted into `datazoo-oauth2`** (from `src/oauth2_*.{hpp,cpp}`): the
authorization-code + PKCE flow, the loopback redirect server that catches the
callback, the cross-platform browser launcher and free-port finder, state/CSRF
validation with timeout handling, and token expiry and refresh bookkeeping.

**Reusable as a pattern:** the DuckDB secret shape — creation providers
(`client_credentials`, `config`, and an interactive `authorization_code` that
was scoped out; see §0 of the implementation plan) — and, most valuably,
the token manager that reads a token out of a secret, refreshes it
transparently when expired, and writes the new tokens back. That machinery is
the difference between "OAuth works once" and "OAuth keeps working".

Critically, `OAuth2Config` already carries `custom_auth_url` /
`custom_token_url` overrides, added precisely so a non-SAP provider could be
plugged in. Google is the second such provider, which is the case the design
already anticipates.

**Genuinely new for Google:** the service-account JWT flow (RFC 7523 — sign an
assertion with the account's private key), which differs from Microsoft's
client-credentials POST; two extra authorization parameters Google requires
before it will issue a refresh token; and the Drive scope strings. See HLD §5.

Two public extensions provide the remaining precedent: the
[`gcs` community extension](https://duckdb.org/community_extensions/extensions/gcs)
([source](https://github.com/northpolesec/duckdb-gcs)) registers a custom
filesystem under its own `gsfs://` prefix, and the
[`gsheets` extension](https://duckdb.org/2025/02/26/google-sheets-community-extension)
performs Google OAuth inside a DuckDB extension. No Google Drive filesystem
extension exists in the
[community list](https://duckdb.org/community_extensions/list_of_extensions);
this would be the first.

**This is now a requirement, not an option** (REQ-A-01/02): the code is
extracted into a shared `datazoo-oauth2` library and **both** extensions
consume it. The residual risks are R-8 and R-9 below.

---

## 11. Effort and decision gate

Roughly **4–6 weeks**, plus standing per-release maintenance (REQ-NF-06):

| Workstream | Estimate |
|---|---|
| `datazoo-oauth2` extraction + `erpl-web` migration (REQ-A-01/02) | 1–1.5 wks |
| Filesystem: resolver, ranged reads, glob, errors | 2–3 wks |
| Google auth on top of the library (endpoints, extra params, SA JWT) | ~0.5 wk |
| Native-format export, write path, CI, docs | 1 wk |

The reuse in §10 removes most of what would have been an auth workstream, but
the extraction adds a workstream of its own — so the honest total is slightly
*higher* than a vendored copy would be, bought in exchange for not maintaining
two OAuth implementations.

**The extraction has value independent of this project.** If Milestone 1 kills
the Drive extension, `erpl-web` still ends up with its OAuth2 code isolated
and independently tested. That is the argument for doing it first rather than
last.

The dominant remaining risk is not authentication — it is path resolution and
quota (R-1, R-2), which is what Milestone 1 exists to measure.

**Recommendation: fund Milestone 1 only (≈1 week).** A throwaway read-only
spike that benchmarks against a FUSE mount and measures metadata
amplification. Commit to the remainder only if the spike shows both a real
advantage over the mount and a tolerable call profile.

**Do not start without a named owner willing to carry REQ-NF-06.**
