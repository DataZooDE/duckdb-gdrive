"""Browser addresses as paths: the parts the SQL suite cannot reach.

* Precedence over httpfs. httpfs claims every https:// address and, loaded
  after this extension, would win a Drive address and fetch it without
  credentials. The unittest runner cannot install httpfs; the CLI can.
* Writing through an address. COPY stages a "tmp_" sibling and moves it onto
  the target; an address names a file by id, so both are the same file. The
  write must land in place -- same id, no second file, nothing deleted.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def _last(out: str) -> str:
    return out.splitlines()[-1]


def test_drive_addresses_win_over_httpfs(sql, gdrive_secret_sql, fixtures_root, drive):
    small = drive.resolve_path("small.csv", root_id=fixtures_root)
    out = sql(f"""{gdrive_secret_sql}
        INSTALL httpfs; LOAD httpfs;
        SELECT sum(amount) FROM read_csv('https://drive.google.com/file/d/{small}/view?usp=sharing');""")
    assert _last(out) == "1500", out


def test_copy_to_an_address_overwrites_in_place(writer, scratch, sql, gdrive_secret_sql):
    file_id = writer.upload("target.csv", b"a\n1\n", scratch, "text/csv")
    url = f"https://drive.google.com/file/d/{file_id}/view?usp=sharing"
    sql(f"""{gdrive_secret_sql}
        COPY (SELECT 7 AS a UNION ALL SELECT 8) TO '{url}' (FORMAT csv);""")

    same_name = [c for c in writer.list_children(scratch) if c["name"] == "target.csv"]
    assert [c["id"] for c in same_name] == [file_id], "one file, the same id"
    assert writer.download(file_id) == b"a\n7\n8\n"


def test_writing_to_an_editor_address_is_refused(sql, gdrive_secret_sql, fixtures_root, drive):
    budget = drive.resolve_path("Budget", root_id=fixtures_root)
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT 1 AS a) TO 'https://docs.google.com/spreadsheets/d/{budget}/edit' (FORMAT csv);""",
              expect_error=True)
    assert "Google Docs editor address" in out, out
