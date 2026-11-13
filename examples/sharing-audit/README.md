# Who can open what: a sharing audit of a Drive folder

One SQL file, no extension besides `gdrive`. It lists every permission on every
file under a folder, joins them with the files' metadata, and writes four
reports back to Drive:

| report | what it lists |
|---|---|
| `exposed.csv` | files and folders anyone with the link (`anonymous`) or the whole domain (`organization`) can open, and whether search finds them |
| `external.csv` | people and groups outside your domain with direct access |
| `stale.csv` | shared files nobody has modified for a year |
| `locked.csv` | locked files (Drive's content restriction): who locked them, when, why, and whether readers may copy them |

1. Point the two globs at the folder to audit (`gdrive://Shared/**`) and the
   reports at a folder of their own (`gdrive://Audit/`). Replace `example.com`
   with your domain.
2. Run it:

   ```bash
   duckdb < audit.sql
   ```

| step | function |
|---|---|
| the files, with owner, last change and lock | `read_gdrive_files('gdrive://Shared/**')` |
| who can reach each one | `gdrive_permissions('gdrive://Shared/**')` |
| the reports | `COPY ... TO 'gdrive://Audit/exposed.csv'` |

`gdrive_permissions` lists what the signed-in identity may see of the
sharing: someone with view access to a file may get a `403` for its
permissions, which stops the audit with an error naming the file. Run it as
the folders' owner, or a Workspace user with access to their sharing
settings. It makes one Drive call per file and folder: a tree of thousands
takes minutes.
