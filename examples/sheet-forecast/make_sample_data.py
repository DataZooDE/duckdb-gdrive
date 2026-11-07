#!/usr/bin/env python3
"""Write sales.csv: three years of synthetic monthly revenue for three
products (product, month, revenue). Import it into Google Sheets, then run
forecast.sql against that Sheet.

    python3 make_sample_data.py

The numbers are made up: a trend, a yearly season and some noise per product
-- the same series as duckdb-sharepoint's excel-forecast example.
"""
import csv
import datetime as dt
import math
import random
from pathlib import Path

random.seed(51)
OUT = Path(__file__).resolve().parent / "sales.csv"
PRODUCTS = {"Widgets": (1200, 15, 0.25), "Gadgets": (800, 25, 0.10), "Gizmos": (400, 5, 0.40)}  # level, trend, season

with OUT.open("w", newline="") as f:
    w = csv.writer(f, lineterminator="\n")
    w.writerow(["product", "month", "revenue"])
    for product, (level, trend, season) in PRODUCTS.items():
        for m in range(36):
            month = dt.date(2023 + m // 12, 1 + m % 12, 1)
            revenue = (level + trend * m) * (1 + season * math.sin(2 * math.pi * m / 12)) * random.uniform(0.93, 1.07)
            w.writerow([product, month.isoformat(), round(revenue, 2)])
print(OUT)
