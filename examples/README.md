# Examples

Each example is one SQL file. Each reads Google Drive live and writes its
result back to Drive, where people already look.

| example | from Drive | with | back to Drive |
|---|---|---|---|
| [sheet-forecast](sheet-forecast/) | a Google Sheet of monthly sales | [anofox_forecast](https://github.com/DataZooDE/anofox-forecast): 12 months ahead per product | `sales-forecast.csv` next to it |
| [sharing-audit](sharing-audit/) | every permission under a folder | `gdrive_permissions` and `read_gdrive_files` | three CSV reports: link-shared, external, stale |

Each needs a `gdrive` secret (see the main README's
[Authentication](../README.md#authentication)). Both were run against a live
Drive with a build of `main` for DuckDB v1.5.6 (`anofox_forecast` from the
community repository). `sharing-audit` needs v2026.11.08 or later:
`gdrive_permissions` and `read_gdrive_files` are not in v2026.10.18.
