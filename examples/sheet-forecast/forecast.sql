-- A Google Sheet -> anofox_forecast -> a Forecast tab in the same Sheet.
--
-- The Sheet (sales.csv, imported into Google Sheets) holds monthly revenue
-- per product. This reads its history tab live through the browser address,
-- forecasts the next 12 months per product, and writes the forecast back into
-- the SAME Sheet, as a tab called Forecast -- in place, through the Sheets
-- API: the Sheet keeps its link, its sharing and its other tabs.

INSTALL gdrive FROM community;
LOAD gdrive;
INSTALL anofox_forecast FROM community;
LOAD anofox_forecast;

-- Your gcloud login, with the Drive scope (see the README's "Authentication"):
--   gcloud auth application-default login --scopes=openid,https://www.googleapis.com/auth/drive
CREATE SECRET (TYPE gdrive);

-- 1. The history, as the Sheet holds it right now. The address is the one in
--    the browser; 'sales' is the tab (Sheets names an imported CSV's tab after
--    the file -- put your own tab name here).
CREATE OR REPLACE TABLE sales AS
SELECT product, month::DATE AS month, revenue::DOUBLE AS revenue
FROM read_sheet_range('https://docs.google.com/spreadsheets/d/YOUR_SHEET_ID/edit', 'sales');

-- 2. Twelve months ahead per product, with a 90% prediction interval.
CREATE OR REPLACE TABLE forecast AS
SELECT product, ds::DATE AS month, round(yhat, 2) AS forecast,
       round(yhat_lower, 2) AS low, round(yhat_upper, 2) AS high, model_name AS model
FROM ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo')
ORDER BY product, month;

-- 3. Back into the Sheet, as its Forecast tab (created on the first run).
--    OVERWRITE clears the tab's old values first, so a re-run never leaves
--    stale rows; months are written as dates, the rest as numbers and text.
COPY forecast TO 'https://docs.google.com/spreadsheets/d/YOUR_SHEET_ID/edit'
    (FORMAT sheet_range, RANGE 'Forecast!A1', OVERWRITE);

SELECT product, min(month) AS first, max(month) AS last, round(sum(forecast)) AS next_12_months
FROM forecast GROUP BY product ORDER BY product;
