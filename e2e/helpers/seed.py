"""Seed the permanent, read-only fixture tree into the CI Shared Drive.

Idempotent: run it as often as you like. A file is re-uploaded only when its
content checksum differs from what Drive holds, so the 100 MB Parquet is
transferred once and then left alone.

    make seed_fixtures

Layout produced under the Shared Drive root::

    /fixtures/
        small.csv                empty.csv
        grüße.csv                "with space.csv"
        nested/a/b/deep.csv
        dup/dup.csv  x2          <- SAME NAME TWICE, on purpose (R-4)
        parts/part-00..09.parquet
        many/f-000..149.csv      <- >100 children, forces pagination
        wide.parquet             <- ~100MB, the REQ-NF-01 benchmark subject
        Budget                   <- native Google Sheet
        Notes                    <- native Google Doc
    /scratch/                    <- per-run folders live here
"""

from __future__ import annotations

import io
import os
import sys
import tempfile
from pathlib import Path

from .drive import Drive, DriveConfigError, FOLDER_MIME
from . import fixtures as fx


def _ensure_path(drive: Drive, path: str, root: str) -> str:
    """mkdir -p, returning the id of the leaf folder."""
    current = root
    for segment in [s for s in path.split("/") if s]:
        current = drive.find_or_create_folder(segment, current)
    return current


def _upload_if_changed(drive: Drive, parent: str, name: str, content: bytes, mime: str) -> str:
    existing = [f for f in drive.list_children(parent, name=name) if f["mimeType"] != FOLDER_MIME]
    if len(existing) == 1:
        want = fx.checksum(content)
        if existing[0].get("md5Checksum") == want:
            return existing[0]["id"]
        drive.delete(existing[0]["id"])
    elif len(existing) > 1:
        # Should only ever happen in the deliberate duplicate folder, which is
        # seeded by a different code path.
        for f in existing:
            drive.delete(f["id"])
    return drive.upload(name, content, parent, mime)


def _make_parquet(rows: list[dict]) -> bytes:
    import duckdb

    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "f.parquet"
        con = duckdb.connect()
        con.execute("CREATE TABLE t AS SELECT * FROM (VALUES " +
                    ",".join(f"({r['part']},{r['i']},{r['v']})" for r in rows) +
                    ") AS v(part, i, v)")
        con.execute(f"COPY t TO '{out}' (FORMAT parquet)")
        con.close()
        return out.read_bytes()


def _make_wide_parquet(target_mb: int = 100) -> bytes:
    """A ~100MB Parquet for the REQ-NF-01 cold-scan benchmark.

    Random-ish, low-compressibility data: a highly compressible file would
    make the download look fast for reasons that have nothing to do with the
    filesystem layer being measured.
    """
    import duckdb

    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "wide.parquet"
        con = duckdb.connect()
        rows = target_mb * 200_000
        con.execute(f"""
            COPY (
                SELECT i AS id,
                       hash(i)              AS h1,
                       hash(i * 7 + 1)      AS h2,
                       hash(i * 13 + 3)     AS h3,
                       (i % 997)::DOUBLE / 7.0 AS d1,
                       md5(i::VARCHAR)      AS s1
                FROM range({rows}) t(i)
            ) TO '{out}' (FORMAT parquet, COMPRESSION zstd)
        """)
        con.close()
        return out.read_bytes()


def seed(drive: Drive) -> dict:
    ids: dict = {}
    fixtures_root = drive.find_or_create_folder(fx.FIXTURES_ROOT)
    drive.find_or_create_folder(fx.SCRATCH_ROOT)
    ids["fixtures"] = fixtures_root

    # --- simple files, incl. nested path, UTF-8 and spaces ----------------
    for path, kind, payload, mime in fx.PERMANENT_LAYOUT:
        parent_path, _, name = path.rpartition("/")
        parent = _ensure_path(drive, parent_path, fixtures_root) if parent_path else fixtures_root
        content = payload()
        if kind == "binary":
            ids[path] = _upload_if_changed(drive, parent, name, content, mime)
        else:
            # Native Sheet/Doc: no md5Checksum to compare against, so create
            # only when absent.
            existing = drive.list_children(parent, name=name)
            if existing:
                ids[path] = existing[0]["id"]
            else:
                ids[path] = drive.upload_native(name, content, parent, mime)
        print(f"  {path} -> {ids[path]}")

    # --- deliberate duplicate names in one folder (R-4) -------------------
    dup_dir = drive.find_or_create_folder("dup", fixtures_root)
    dups = [f for f in drive.list_children(dup_dir, name="dup.csv")]
    if len(dups) != 2:
        for f in dups:
            drive.delete(f["id"])
        drive.upload("dup.csv", fx.dup_csv("first"), dup_dir, "text/csv")
        drive.upload("dup.csv", fx.dup_csv("second"), dup_dir, "text/csv")
    print(f"  dup/dup.csv x2 -> {dup_dir}")
    ids["dup_dir"] = dup_dir

    # --- glob parts -------------------------------------------------------
    parts_dir = drive.find_or_create_folder("parts", fixtures_root)
    for p in range(fx.GLOB_PARTS):
        name = f"part-{p:02d}.parquet"
        _upload_if_changed(drive, parts_dir, name, _make_parquet(fx.part_parquet_rows(p)),
                           "application/octet-stream")
    print(f"  parts/part-00..{fx.GLOB_PARTS - 1:02d}.parquet -> {parts_dir}")
    ids["parts_dir"] = parts_dir

    # --- pagination: more children than Drive's default page size ---------
    many_dir = drive.find_or_create_folder("many", fixtures_root)
    have = {f["name"] for f in drive.list_children(many_dir)}
    for i in range(fx.PAGINATION_COUNT):
        name = f"f-{i:03d}.csv"
        if name not in have:
            drive.upload(name, f"i\n{i}\n".encode(), many_dir, "text/csv")
    print(f"  many/f-000..{fx.PAGINATION_COUNT - 1:03d}.csv -> {many_dir}")
    ids["many_dir"] = many_dir

    # --- the benchmark subject -------------------------------------------
    if os.environ.get("GDRIVE_SEED_SKIP_WIDE") != "1":
        existing = [f for f in drive.list_children(fixtures_root, name="wide.parquet")]
        if not existing:
            print("  wide.parquet: generating ~100MB (once) ...")
            _upload_if_changed(drive, fixtures_root, "wide.parquet",
                               _make_wide_parquet(), "application/octet-stream")
        ids["wide.parquet"] = (existing[0]["id"] if existing
                               else drive.resolve_path("fixtures/wide.parquet"))
        print(f"  wide.parquet -> {ids['wide.parquet']}")
    else:
        print("  wide.parquet: SKIPPED (GDRIVE_SEED_SKIP_WIDE=1)")

    return ids


def main() -> int:
    try:
        # Seeding WRITES, so it must run as the delegated user: a service
        # account has no Drive storage quota and gets 403 on every upload
        # outside a Shared Drive.
        drive = Drive.from_env(prefer_user=True)
    except DriveConfigError as e:
        print(f"cannot seed: {e}", file=sys.stderr)
        return 2
    print(f"seeding Shared Drive {drive.drive_id} ...")
    seed(drive)
    print("done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
