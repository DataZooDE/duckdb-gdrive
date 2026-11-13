---
name: gdrive-sheets
description: >
  Google Sheets by tab and A1 range with the gdrive DuckDB extension:
  read_sheet_range(path, 'Tab!A1:F' [, header, all_varchar]) reads any tab or
  range, typed (dates, booleans, formula results); COPY ... TO '<Sheet>'
  (FORMAT sheet_range, RANGE 'Tab!A1' [, HEADER b] [, OVERWRITE | APPEND]
  [, CREATE b]) writes into a tab in place through the Sheets API (missing
  tab created, other tabs and formats kept), and creates a new native Sheet
  when a gdrive:// path names nothing. Covers A1 forms and quoting ('My Tab'!A1),
  OVERWRITE vs APPEND vs the default block write, atomicity and the 2 MB
  split, value typing (dates as dates, '=1+1' stays text, LIST refused), the
  drive scope and SERVICE_DISABLED errors. Use when reading a specific tab or
  range of a Sheet, writing query results or a forecast into a Sheet, or
  appending rows to one. (The separate gsheets community extension is a
  different product with its own functions.)
version: 2026.11.14
user-invocable: false
---

# gdrive extension — Sheets by tab and range

New in v2026.11.13 (not in v2026.11.08 or earlier).

`read_csv` on a Sheet exports its **first tab** as text. For any tab, a range,
typed columns, or **writing into** a Sheet, use the Sheets-API surface:

```sql
SELECT * FROM read_sheet_range('gdrive://Finance/sales', 'Actuals!A1:F');   -- A..F, to the last row
SELECT * FROM read_sheet_range('gdrive://Finance/sales', 'Actuals');        -- whole tab
SELECT * FROM read_sheet_range('https://docs.google.com/spreadsheets/d/<id>/edit', '''Q3 plan''!B2:D20', header := false);

COPY forecast TO 'gdrive://Finance/sales' (FORMAT sheet_range, RANGE 'Forecast!A1', OVERWRITE);
COPY new_rows TO 'gdrive://id:<id>'       (FORMAT sheet_range, RANGE 'Log!A1', APPEND);
COPY summary  TO 'gdrive://Finance/2026/q3-summary' (FORMAT sheet_range, RANGE 'Summary!A1');  -- creates it
```

Paths: `gdrive://…` (scheme optional for `read_sheet_range`), `gdrive://id:<id>`, or the
browser address. An existing target must be a **native Google Sheet** (a CSV/xlsx in Drive is
refused, never replaced).

## Creating a Sheet (new in v2026.11.14; not in v2026.11.13 or earlier)
- A `gdrive://` **path that names nothing** gets a new native Sheet (missing folders too);
  its one tab is renamed to the RANGE's tab, written in the same atomic request. `OVERWRITE` /
  `APPEND` into a new Sheet = a block at the anchor, header included. `CREATE false` opts out
  ("no such Sheet"); `CREATE` takes only true/false.
- `gdrive://id:` and browser addresses never create: they must name an existing Sheet.
- Names ending `.csv .tsv .parquet .json .jsonl .ndjson .xlsx .xls .txt` (any case) are refused
  for creation — "Drop the extension for a Sheet, or use FORMAT csv / FORMAT parquet". An
  existing Sheet with such a name is written into.
- Created LAST (after the query finished and the block fit), so a failing query creates nothing.
  A Sheets error after the create leaves the Sheet in place; the error names its id and says
  "left in place empty" (or partially written) — re-run the COPY to write into it.
- Sent once, never retried (Drive refuses reserved ids for native files); a lost answer is
  looked up by a marker. "may not have been created; nothing was retried; it is safe to re-run"
  means the lookup found nothing. Two writers creating the same Sheet at once can make two.
- Cost: one `files_create`, one `sheets_update`, no `sheets_get`. duckdb-sharepoint's
  `excel_range` does not create workbooks.

## Ranges (A1, tab required)
`Tab!A1:F200`, `Tab!A1:F` (open rows), `Tab!A:F`, `Tab!2:5`, bare `Tab`, `Tab!B2` (a cell /
the write anchor). Quote a tab with spaces or quotes: `'Q3 plan'!A1`, `'It''s'!A1` — inside a
SQL string literal double the quotes: `'''Q3 plan''!A1'`. `A1:B2` with no tab is an error.

## Reading
- First row = header unless `header := false` (`column1`, `column2`, …); empty / duplicate
  headers become `columnN` / `name_2`. `all_varchar := true` reads text.
- Types like duckdb-sharepoint's `read_excel_range`: numbers → BIGINT/DOUBLE, date/time
  formatted cells → DATE/TIMESTAMP/TIME, booleans → BOOLEAN, mixed → VARCHAR; an error cell
  (`#DIV/0!`) is NULL in a typed column. Formulas read as their results. Trailing empty rows
  are dropped. One `spreadsheets.get` per call (`sheets_get` in `gdrive_stats()`).

## Writing (in place: id, link, sharing, other tabs kept)
| option | effect |
|---|---|
| `RANGE 'Tab!A1'` | required. Anchor = top-left cell; missing tab is created. Explicit bounds (`'Tab!A1:F'`) must hold the block. |
| `HEADER true\|false` | column names as first row (default true) |
| *(none)* | write the block from the anchor; nothing else cleared — a shorter result leaves old rows below |
| `OVERWRITE` | clear values from the anchor down/right (to the tab edge, or the explicit bounds), then write |
| `APPEND` | `values.append` below the data at the anchor; header only for a new tab or explicit `HEADER true` |

- Values only: formats inside the range and formulas outside it survive. Values are typed:
  DATE/TIMESTAMP/TIME become Sheets dates (read back as dates), `'=1+1'` stays text (RAW),
  NULL = empty cell, DECIMAL/HUGEINT → DOUBLE. LIST/STRUCT/MAP columns are refused — use
  `to_json(col)` or `array_to_string(col, ',')`.
- ≤ ~2 MB of cell data = ONE atomic `batchUpdate` (new tab + grid growth + clear + cells).
  Larger writes are split and **not atomic**: on failure the error says "partially written",
  re-run with `OVERWRITE`. Rows are buffered until the query finishes, so a failing query
  writes nothing and the Sheet is never trashed.
- 10M cells per spreadsheet is enforced up front. `OVERWRITE` and `APPEND` exclude each other;
  `USE_TMP_FILE` is refused. Plain `COPY … (FORMAT csv)` onto a Sheet stays refused.
- APPEND is never retried (a repeat appends twice); OVERWRITE batches are.

## Errors
- `insufficient OAuth scope … sheet_range`: writing needs `DRIVE_SCOPE
  'https://www.googleapis.com/auth/drive'`; `drive.readonly` reads but cannot write.
- `… is not enabled in the Google Cloud project …`: run the printed
  `gcloud services enable sheets.googleapis.com --project <project>`.
- `has no tab 'X'`: check the tab name (case and spaces matter).
- `no such Sheet` on a COPY: an `id:`/address of nothing, or `CREATE false`.
- `appeared while the COPY ran`: something took the name between the check and the create;
  re-run to write into it if it is a Sheet.
