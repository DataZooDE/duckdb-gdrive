# Google Sheet → anofox_forecast → back to Drive

One SQL file that reads monthly sales from a Google Sheet, forecasts the next
12 months per product with
[anofox_forecast](https://github.com/DataZooDE/anofox-forecast), and writes
the forecast to Drive next to it. The Sheet is read as it is right now: there
is no export step to keep in sync.

1. Make the sample data (synthetic: three products, 2023–2025), or use your
   own Sheet with the columns `product`, `month` and `revenue`:

   ```bash
   python3 make_sample_data.py   # writes sales.csv
   ```

   A ready-made `sales.csv` is checked in next to it.

2. In Google Sheets: *File → Import → Upload* `sales.csv`, *Create new
   spreadsheet*. Copy the Sheet's address from the browser.
3. Put that address into `forecast.sql` (it replaces
   `https://docs.google.com/spreadsheets/d/YOUR_SHEET_ID/edit`), and the
   folder to write to (`gdrive://Finance/`), then run it:

   ```bash
   duckdb < forecast.sql
   ```

Drive gets `sales-forecast.csv` with `product`, `month`, `forecast`, `low`,
`high` (a 90% prediction interval) and `model`. Running it again replaces the
file. Open it with Google Sheets, or import it into the Sheet as a new tab.

| step | function |
|---|---|
| read the Sheet | `read_csv('https://docs.google.com/spreadsheets/d/<id>/edit')` |
| forecast per product | `ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo')` |
| write it back | `COPY forecast TO 'gdrive://Finance/sales-forecast.csv' (FORMAT csv, HEADER)` |

Drive exports a Sheet's **first tab** as CSV. Keep the history on the first
tab, or give it a Sheet of its own.
