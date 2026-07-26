"""Fixture layout and content.

Two tiers, for a reason (plan section 2.2):

* **Permanent, read-only** under ``/fixtures``. Seeded once by ``make
  seed_fixtures``. Re-uploading a 100 MB Parquet on every CI run would be slow
  and pointless, and read-only tests cannot disturb each other.
* **Per-run scratch** under ``/scratch/run-<uuid>``. Every mutating test gets
  its own, and deletes it in teardown. This is what makes concurrent CI runs
  safe against a single Shared Drive.

The fixture *contents* are chosen to make specific failures impossible to miss:
duplicate names (R-4), >100 children (pagination), native Google formats
(REQ-F-07), non-ASCII names, and an empty file.
"""

from __future__ import annotations

import csv
import io
import hashlib

FIXTURES_ROOT = "fixtures"
SCRATCH_ROOT = "scratch"

#: Liveness marker written inside every scratch folder. Drive does not advance
#: a FOLDER's modifiedTime when its children change, so the sweeper would
#: otherwise judge a busy run idle and delete it mid-test. The heartbeat file's
#: own modifiedTime is the thing that actually moves.
HEARTBEAT_NAME = ".heartbeat"

SHEET_MIME = "application/vnd.google-apps.spreadsheet"
DOC_MIME = "application/vnd.google-apps.document"

#: Number of files in the pagination fixture folder. Must exceed Drive's
#: default page size of 100, or the test proves nothing.
PAGINATION_COUNT = 150

#: Number of parts in the glob fixture folder.
GLOB_PARTS = 10


def small_csv() -> bytes:
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["id", "name", "amount"])
    for i in range(1, 6):
        w.writerow([i, f"row-{i}", i * 100])
    return buf.getvalue().encode()


def deep_csv() -> bytes:
    return b"a,b\n1,2\n3,4\n"


def dup_csv(variant: str) -> bytes:
    """Two files with the SAME name in one folder (R-4).

    Contents differ so a test can prove the resolver refused to guess rather
    than silently picking whichever Drive happened to return first.
    """
    return f"variant\n{variant}\n".encode()


def utf8_csv() -> bytes:
    return "grüße,measure\nÜbergröße,42\n".encode("utf-8")


def sheet_source() -> bytes:
    """CSV uploaded with conversion to a native Sheet (REQ-F-07)."""
    return b"quarter,revenue\nQ1,1000\nQ2,2000\nQ3,3000\n"


def doc_source() -> bytes:
    return b"This is a Google Doc fixture.\nSecond line.\n"


def part_parquet_rows(part: int) -> list[dict]:
    return [{"part": part, "i": i, "v": part * 1000 + i} for i in range(10)]


def checksum(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


#: Permanent read-only fixture tree. Paths are relative to /fixtures.
#: (path, kind, payload-callable) -- kind drives how it is uploaded.
PERMANENT_LAYOUT = [
    ("small.csv", "binary", small_csv, "text/csv"),
    ("empty.csv", "binary", lambda: b"", "text/csv"),
    ("grüße.csv", "binary", utf8_csv, "text/csv"),
    ("with space.csv", "binary", small_csv, "text/csv"),
    ("nested/a/b/deep.csv", "binary", deep_csv, "text/csv"),
    ("Budget", "sheet", sheet_source, SHEET_MIME),
    ("Notes", "doc", doc_source, DOC_MIME),
]
