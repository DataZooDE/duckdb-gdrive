# Google Sheet → anofox_forecast → a Forecast tab in the same Sheet

One SQL file that reads monthly sales from a Google Sheet, forecasts the next
12 months per product with
[anofox_forecast](https://github.com/DataZooDE/anofox-forecast), and writes
the forecast back into the same Sheet, as a `Forecast` tab. The Sheet is read
as it is right now, and written in place: there is no export step to keep in
sync, and the Sheet keeps its link, its sharing and its other tabs.

1. Make the sample data (synthetic: three products, 2023–2025), or use your
   own Sheet with the columns `product`, `month` and `revenue`:

   ```bash
   python3 make_sample_data.py   # writes sales.csv
   ```

   A ready-made `sales.csv` is checked in next to it.

2. In Google Sheets: *File → Import → Upload* `sales.csv`, *Create new
   spreadsheet*. Copy the Sheet's address from the browser.
3. Put that address into `forecast.sql` (it replaces
   `https://docs.google.com/spreadsheets/d/YOUR_SHEET_ID/edit`, twice) and,
   if yours differs, the history tab's name (`'sales'`), then run it:

   ```bash
   duckdb < forecast.sql
   ```

The Sheet gets a `Forecast` tab with `product`, `month` (as dates),
`forecast`, `low`, `high` (a 90% prediction interval) and `model`. Running it
again replaces the tab's values (`OVERWRITE`) and leaves its formatting, and
every other tab, alone. Writing needs the full Drive scope and the Sheets API
enabled in your Google Cloud project (`gcloud services enable
sheets.googleapis.com`).

| step | function |
|---|---|
| read the Sheet's history tab | `read_sheet_range('https://docs.google.com/spreadsheets/d/<id>/edit', 'sales')` |
| forecast per product | `ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo')` |
| write it back | `COPY forecast TO '<address>' (FORMAT sheet_range, RANGE 'Forecast!A1', OVERWRITE)` |

Prefer a file? `COPY forecast TO 'gdrive://Finance/sales-forecast.csv' (FORMAT
csv, HEADER)` writes one next to the Sheet instead.
