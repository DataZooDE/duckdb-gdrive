# Exploratory testing log

Time-boxed sessions against **real** Google Drive (no mocks), run alongside
the sibling `duckdb-sharepoint`. Each confirmed bug got a failing live or e2e
test first, then a fix in its own PR.

## Session 2026-10-10 (with duckdb-sharepoint)

| charter | finding | outcome |
|---|---|---|
| Load both siblings into one DuckDB | Second `LOAD` failed: `Scalar Function with name "remove_file" already exists!` (either order) | Fixed: shared VFS functions register with `IGNORE_ON_CONFLICT`; the two copies are now identical code (`scripts/check_vfs_parity.sh`). |
| Names that are also glob patterns | `report[1].csv` could not be read by its own path ("No files found") | Fixed: a pattern matching nothing falls back to the literal path, as DuckDB's local filesystem does. |
| Secrets scoped to a subfolder | `write_blob` walked parents from the root and asked about `gdrive://scratch`, which no secret covered | Fixed: parents are created bottom-up from the nearest existing folder. |
| ATTACH a `.duckdb` file on Drive | DuckDB 1.5 canonicalised `gdrive://` to `gdrive:/`, then opened it without a client connection | Fixed: `CanonicalizePath` override (1.5+) and secret/setting lookup through the `FileOpener`. |
| Block cache across sessions | Block size missing from the key → spurious "short read" when two sessions use different `gdrive_block_size_bytes`; a fetch outliving `Clear()` could account to a newer entry | Fixed in the now-pure, unit-tested `gdrive_blockcache.*`. |
| Cache identity | Path/block caches keyed by secret *name*; identity then excluded `ACCESS_TOKEN` for every provider and ignored the credential file actually used | Fixed: identity material is a tested pure function covering the real principal. |
| e2e harness leaks | pytest tracebacks printed the secret fixture; `-c` put `CREATE SECRET` in argv | Fixed: redacted repr, SQL over stdin, scrubbed failures. |

## Cross-check with duckdb-sharepoint

One scenario script (write/size/overwrite, folder and absent handling,
idempotent remove, move onto existing, glob of folder / brackets / braces,
`read_csv` of an absent file, COPY re-run, append, missing secret) ran against
both extensions and the outcomes were diffed. gdrive divergences found and
fixed: `file_size` of a folder returned `0`; `{a,b}` alternation with literal
expansions matched nothing. Remaining, intentional: a missing secret raises
`IOException` here and `InvalidInputException` in sharepoint.
