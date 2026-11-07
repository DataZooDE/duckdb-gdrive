-- A Google Sheet -> anofox_forecast -> a forecast next to it in Drive.
--
-- The Sheet (sales.csv, imported into Google Sheets) holds monthly revenue
-- per product. This reads it live through its browser address, forecasts the
-- next 12 months per product, and writes the forecast to Drive as a CSV that
-- Sheets opens directly.

INSTALL gdrive FROM community;
LOAD gdrive;
INSTALL anofox_forecast FROM community;
LOAD anofox_forecast;

-- Your gcloud login, with the Drive scope (see the README's "Authentication"):
--   gcloud auth application-default login --scopes=openid,https://www.googleapis.com/auth/drive
CREATE SECRET (TYPE gdrive);

-- 1. The history, as the Sheet holds it right now. The address is the one in
--    the browser; Drive exports the Sheet's first tab as CSV.
CREATE OR REPLACE TABLE sales AS
SELECT product, month::DATE AS month, revenue::DOUBLE AS revenue
FROM read_csv('https://docs.google.com/spreadsheets/d/YOUR_SHEET_ID/edit');

-- 2. Twelve months ahead per product, with a 90% prediction interval.
CREATE OR REPLACE TABLE forecast AS
SELECT product, ds::DATE AS month, round(yhat, 2) AS forecast,
       round(yhat_lower, 2) AS low, round(yhat_upper, 2) AS high, model_name AS model
FROM ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo')
ORDER BY product, month;

-- 3. Back to Drive, next to the Sheet. A re-run replaces the file.
COPY forecast TO 'gdrive://Finance/sales-forecast.csv' (FORMAT csv, HEADER);

SELECT product, min(month) AS first, max(month) AS last, round(sum(forecast)) AS next_12_months
FROM forecast GROUP BY product ORDER BY product;
