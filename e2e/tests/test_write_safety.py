"""Writes never destroy what they did not mean to replace.

Found by exploratory testing (2026-11-07), and here because each needs the
harness: a native Sheet in scratch, or a file id that SQL cannot see.

* COPY by path onto a native Sheet used to move a binary file over it and
  send the Sheet to the trash; an id write replaced the Sheet's content.
* An id kept for a file that was since trashed (COPY's default staging
  replaces the file) still read the old, trashed content.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live

DRIVE_V3 = "https://www.googleapis.com/drive/v3"
SHEET = "application/vnd.google-apps.spreadsheet"


def _state(drive, file_id: str) -> dict:
    resp = drive._request("GET", f"{DRIVE_V3}/files/{file_id}", "probe.get",
                          params={"fields": "id,trashed,mimeType"})
    resp.raise_for_status()
    return resp.json()


def test_copy_by_path_onto_a_sheet_is_refused(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    sheet = writer.upload_native("plan", b"a,b\n1,2\n", scratch, SHEET)
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT 9 AS a, 9 AS b) TO 'gdrive://scratch/{name}/plan' (FORMAT csv);""", expect_error=True)
    assert "native Google file" in out, out
    assert _state(writer, sheet) == {"id": sheet, "trashed": False, "mimeType": SHEET}
    # Nothing left behind: not the tmp file, not a second "plan".
    names = sorted(c["name"] for c in writer.list_children(scratch) if c["name"] != ".heartbeat")
    assert names == ["plan"], names


def test_id_write_onto_a_sheet_is_refused(writer, scratch, sql, gdrive_secret_sql):
    sheet = writer.upload_native("plan2", b"a,b\n1,2\n", scratch, SHEET)
    for stmt in (f"SELECT write_blob('gdrive://id:{sheet}', encode('x'));",
                 f"COPY (SELECT 1 AS a) TO 'gdrive://id:{sheet}' (FORMAT csv, USE_TMP_FILE false);"):
        out = sql(f"{gdrive_secret_sql}\n{stmt}", expect_error=True)
        assert "native Google file" in out, out
    out = sql(f"""{gdrive_secret_sql}
        SELECT count(*) FROM read_csv('gdrive://id:{sheet}');""")
    assert out.splitlines()[-1] == "1", out   # still the Sheet's one data row


def test_a_trashed_file_is_gone_by_id_too(writer, scratch, sql, gdrive_secret_sql):
    file_id = writer.upload("t.csv", b"a\n1\n", scratch, "text/csv")
    writer.delete(file_id, permanent=False)
    out = sql(f"""{gdrive_secret_sql}
        SELECT * FROM read_csv('gdrive://id:{file_id}');""", expect_error=True)
    assert "No files found" in out, out
    out = sql(f"""{gdrive_secret_sql}
        SELECT remove_file('gdrive://id:{file_id}');""")
    assert out.splitlines()[-1] == "false", out
    out = sql(f"""{gdrive_secret_sql}
        SELECT write_blob('gdrive://id:{file_id}', encode('x'));""", expect_error=True)
    assert "in the trash" in out, out
