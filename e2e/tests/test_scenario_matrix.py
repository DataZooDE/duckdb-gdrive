"""The scenario matrix shared with duckdb-sharepoint's e2e/: byte-exact round
trips across block boundaries, the formats DuckDB layers on the filesystem,
and a multi-threaded scan that must equal a serial one while the block cache's
single-flight keeps fetched bytes within the files' size."""

from __future__ import annotations

import hashlib
import os

import pytest

pytestmark = pytest.mark.live

BLOCK = 262144  # forced as gdrive_block_size_bytes so reads cross blocks


def _base(writer, scratch_id: str) -> str:
    return f"gdrive://scratch/{writer.get_metadata(scratch_id)['name']}"


@pytest.mark.parametrize("size", [0, 1, BLOCK - 1, BLOCK, BLOCK + 1, 3 * BLOCK + 17])
def test_round_trip_is_byte_exact(writer, scratch, sql, gdrive_secret_sql, size, tmp_path):
    data = os.urandom(size)
    local = tmp_path / "in.bin"
    local.write_bytes(data)
    uri = f"{_base(writer, scratch)}/blob_{size}.bin"
    out = sql(f"{gdrive_secret_sql} SET gdrive_block_size_bytes = {BLOCK};"
              f" SELECT write_blob('{uri}', content) FROM read_blob('{local}');"
              f" SELECT file_size('{uri}'), md5(content) FROM read_blob('{uri}');")
    assert out.splitlines()[-1] == f"{size}|{hashlib.md5(data).hexdigest()}"


def test_formats_round_trip(writer, scratch, sql, gdrive_secret_sql):
    base = _base(writer, scratch)
    out = sql(f"""{gdrive_secret_sql}
        INSTALL json; LOAD json;
        COPY (SELECT i, 'r' || i AS s FROM range(500) t(i)) TO '{base}/f.csv' (HEADER);
        COPY (SELECT i, 'r' || i AS s FROM range(500) t(i)) TO '{base}/f.json';
        COPY (SELECT i, 'r' || i AS s FROM range(500) t(i)) TO '{base}/f.parquet';
        SELECT (SELECT sum(i) FROM read_csv('{base}/f.csv')) || '/' ||
               (SELECT sum(i) FROM read_json('{base}/f.json')) || '/' ||
               (SELECT sum(i) FROM read_parquet('{base}/f.parquet'));""")
    assert out.splitlines()[-1] == "124750/124750/124750"


def test_union_by_name_across_files(writer, scratch, sql, gdrive_secret_sql):
    base = _base(writer, scratch)
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT 1 AS a, 2 AS b) TO '{base}/ubn_x.parquet';
        COPY (SELECT 3 AS a, 'z' AS c) TO '{base}/ubn_y.parquet';
        SELECT count(*), count(b), count(c) FROM read_parquet('{base}/ubn_*.parquet', union_by_name = true);""")
    assert out.splitlines()[-1] == "2|1|1"


def test_parallel_scan_matches_serial_and_bounds_fetches(writer, scratch, sql, gdrive_secret_sql):
    base = f"{_base(writer, scratch)}/par"
    sql(f"""{gdrive_secret_sql}
        COPY (SELECT i, i % 7 AS k, md5(i::VARCHAR) AS h FROM range(400000) t(i))
          TO '{base}' (FORMAT parquet, PARTITION_BY (k), ROW_GROUP_SIZE 20000);""")
    q = f"SELECT count(*), sum(i), count(DISTINCT h) FROM read_parquet('{base}/**/*.parquet')"
    serial = sql(f"{gdrive_secret_sql} SET threads = 1; {q};").splitlines()[-1]
    out = sql(f"""{gdrive_secret_sql} SET threads = 16; SET gdrive_block_size_bytes = 1048576;
        CALL gdrive_reset_stats(); {q};
        SELECT value FROM gdrive_stats() WHERE metric = 'files_media';""").splitlines()
    assert out[-2] == serial == "400000|79999800000|400000"
    media_calls = int(out[-1])
    total = int(sql(f"{gdrive_secret_sql} SELECT sum(file_size(file)) FROM glob('{base}/**/*.parquet');"
                    ).splitlines()[-1])
    # Single-flight: each 1-MiB block is fetched at most once.
    assert media_calls <= total // 1048576 + 7 * 2, f"{media_calls} downloads for {total} bytes"
