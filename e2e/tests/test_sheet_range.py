"""COPY ... (FORMAT sheet_range) changes a Sheet in place and nothing else.

Here rather than in SQLLogicTest because each check needs the Sheets API
from the harness: to plant a formula or a cell format the extension cannot
write, and to read the Sheet back independently of the code under test.

* A COPY that fails -- in the query, or in the writer's own finalize --
  leaves the Sheet exactly as it was, and NOT in the trash. On DuckDB 1.5.6 a
  failed COPY calls TryRemoveFile on its target; for an in-place writer that
  target is the Sheet itself.
* A formula on another tab, a formula beside the range, and a cell format
  inside it survive an OVERWRITE.
"""

from __future__ import annotations

import pytest

from helpers import fixtures as fx

pytestmark = pytest.mark.live

DRIVE_V3 = "https://www.googleapis.com/drive/v3"
SHEET = "application/vnd.google-apps.spreadsheet"


def _drive_state(drive, file_id: str) -> dict:
    resp = drive._request("GET", f"{DRIVE_V3}/files/{file_id}", "probe.get",
                          params={"fields": "id,trashed,mimeType"})
    resp.raise_for_status()
    return resp.json()


def _values(drive, sheet: str, rng: str) -> list:
    return drive.sheets("GET", f"{sheet}/values/{rng}").get("values", [])


def _cells(drive, sheet: str, rng: str) -> list:
    """userEnteredValue / effectiveValue / bold of every cell in `rng`."""
    got = drive.sheets("GET", sheet, params={
        "ranges": rng, "includeGridData": "true",
        "fields": "sheets(data(rowData(values(userEnteredValue,effectiveValue,"
                  "userEnteredFormat/textFormat/bold))))"})
    rows = got["sheets"][0]["data"][0].get("rowData", [])
    return [r.get("values", []) for r in rows]


def _tab_id(drive, sheet: str, title: str) -> int:
    props = drive.sheets("GET", sheet, params={"fields": "sheets.properties(sheetId,title)"})
    for s in props["sheets"]:
        if s["properties"]["title"] == title:
            return s["properties"].get("sheetId", 0)
    raise AssertionError(f"no tab {title}")


def _plant(drive, sheet: str) -> None:
    """A Calc tab with a formula over Sheet1, a formula in Sheet1!D1 beside
    the a,b block, and bold on Sheet1!A2."""
    drive.sheets("POST", f"{sheet}:batchUpdate", {"requests": [{"addSheet": {"properties": {"title": "Calc"}}}]})
    drive.sheets("PUT", f"{sheet}/values/Calc!A1", {"values": [["=SUM(Sheet1!A2:B2)"]]},
                 params={"valueInputOption": "USER_ENTERED"})
    drive.sheets("PUT", f"{sheet}/values/Sheet1!D1", {"values": [["=A2*10"]]},
                 params={"valueInputOption": "USER_ENTERED"})
    tab = _tab_id(drive, sheet, "Sheet1")
    drive.sheets("POST", f"{sheet}:batchUpdate", {"requests": [{"repeatCell": {
        "range": {"sheetId": tab, "startRowIndex": 1, "endRowIndex": 2, "startColumnIndex": 0, "endColumnIndex": 1},
        "cell": {"userEnteredFormat": {"textFormat": {"bold": True}}},
        "fields": "userEnteredFormat.textFormat.bold"}}]})


def test_a_failed_copy_leaves_the_sheet_intact(writer, scratch, sql, gdrive_secret_sql):
    sheet = writer.upload_sheet("target", b"a,b\n1,2\n", scratch)
    _plant(writer, sheet)
    before = _values(writer, sheet, "Sheet1!A1:D2")
    assert before == [["a", "b", "", "10"], ["1", "2"]], before
    failures = {
        # The query fails mid-stream, after the writer was initialised: rows
        # are buffered, so nothing was sent.
        "boom": f"""COPY (SELECT CASE WHEN i < 9000 THEN i ELSE error('boom') END AS a FROM range(10000) t(i))
                    TO 'gdrive://id:{sheet}' (FORMAT sheet_range, RANGE 'Sheet1!A1', OVERWRITE);""",
        # The writer's own finalize refuses (more rows than the range holds):
        # the failure DuckDB cleans up after by removing the "created" file.
        "do not fit": f"""COPY (SELECT i AS a FROM range(5) t(i))
                          TO 'gdrive://id:{sheet}' (FORMAT sheet_range, RANGE 'Sheet1!A1:A3', OVERWRITE);""",
    }
    for expected, stmt in failures.items():
        out = sql(f"{gdrive_secret_sql}\n{stmt}", expect_error=True)
        assert expected in out, out
        assert _drive_state(writer, sheet) == {"id": sheet, "trashed": False, "mimeType": SHEET}
        assert _values(writer, sheet, "Sheet1!A1:D2") == before
        assert _values(writer, sheet, "Calc!A1") == [["3"]]


def test_a_failed_copy_to_a_bare_path_leaves_a_local_namesake_alone(
        writer, scratch, sql, gdrive_secret_sql, tmp_path, monkeypatch):
    """A bare target ('scratch/run-x/target', no gdrive://) is a Drive path to
    the writer, but DuckDB's failed-COPY cleanup gets the path as written and
    hands a scheme-less one to the LOCAL filesystem -- which deletes a local
    file of the same relative name. Found in duckdb-sharepoint (#131), where it
    deleted the file for real. The rewrite now gives a bare target its scheme,
    so the cleanup reaches GDriveFileSystem::TryRemoveFile instead."""
    sheet = writer.upload_sheet("target", b"a,b\n1,2\n", scratch)
    run = writer._request("GET", f"https://www.googleapis.com/drive/v3/files/{scratch}", "files_get",
                          params={"fields": "name", "supportsAllDrives": "true"}).json()["name"]
    bare = f"{fx.SCRATCH_ROOT}/{run}/target"
    monkeypatch.chdir(tmp_path)
    local = tmp_path / bare
    local.parent.mkdir(parents=True)
    local.write_bytes(b"local data, not the Sheet\n")
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT i AS a FROM range(5) t(i)) TO '{bare}' (FORMAT sheet_range, RANGE 'Sheet1!A1:A3', OVERWRITE);""",
              expect_error=True)
    assert "do not fit" in out, out
    assert local.read_bytes() == b"local data, not the Sheet\n", "the failed COPY deleted a local file"
    assert _drive_state(writer, sheet) == {"id": sheet, "trashed": False, "mimeType": SHEET}


def test_overwrite_keeps_formulas_and_formats_outside_its_values(writer, scratch, sql, gdrive_secret_sql):
    sheet = writer.upload_sheet("keep", b"a,b\n1,2\n", scratch)
    _plant(writer, sheet)
    sql(f"""{gdrive_secret_sql}
        COPY (SELECT 5 AS a, 6 AS b) TO 'gdrive://id:{sheet}' (FORMAT sheet_range, RANGE 'Sheet1!A1:B', OVERWRITE);""")
    # The block is the new data; the formula beside it (D1) and on the other
    # tab are still formulas, and now compute over the new values.
    assert _values(writer, sheet, "Sheet1!A1:B3") == [["a", "b"], ["5", "6"]]
    d1 = _cells(writer, sheet, "Sheet1!D1")[0][0]
    assert d1["userEnteredValue"] == {"formulaValue": "=A2*10"}, d1
    assert d1["effectiveValue"] == {"numberValue": 50}, d1
    calc = _cells(writer, sheet, "Calc!A1")[0][0]
    assert calc["userEnteredValue"] == {"formulaValue": "=SUM(Sheet1!A2:B2)"}, calc
    assert calc["effectiveValue"] == {"numberValue": 11}, calc
    # The cell format inside the range: values were written, formats kept.
    a2 = _cells(writer, sheet, "Sheet1!A2")[0][0]
    assert a2["userEnteredFormat"]["textFormat"]["bold"] is True, a2
    # A string that looks like a formula is written as text (RAW).
    sql(f"""{gdrive_secret_sql}
        COPY (SELECT '=1+1' AS f) TO 'gdrive://id:{sheet}' (FORMAT sheet_range, RANGE 'Text!A1');""")
    f = _cells(writer, sheet, "Text!A2")[0][0]
    assert f["userEnteredValue"] == {"stringValue": "=1+1"}, f


# ---------------------------------------------------------------------------
# Creating a Sheet at a path that names nothing (2026-11-13). The create
# happens in the writer's finalize, after every row is in, so a COPY whose
# query fails creates nothing; a Sheets error AFTER the create leaves the
# new Sheet in place (user decision), named in the error, and a re-run writes
# into it.
# ---------------------------------------------------------------------------

def _named(drive, folder: str, name: str) -> list[dict]:
    return [c for c in drive.list_children(folder) if c["name"] == name]


def _run_name(drive, scratch: str) -> str:
    return drive._request("GET", f"{DRIVE_V3}/files/{scratch}", "files_get",
                          params={"fields": "name", "supportsAllDrives": "true"}).json()["name"]


def test_a_failed_query_creates_no_sheet(writer, scratch, sql, gdrive_secret_sql):
    run = _run_name(writer, scratch)
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT CASE WHEN i < 9000 THEN i ELSE error('boom') END AS a FROM range(10000) t(i))
        TO 'gdrive://{fx.SCRATCH_ROOT}/{run}/never/made' (FORMAT sheet_range, RANGE 'T!A1');""",
              expect_error=True)
    assert "boom" in out, out
    # Neither the Sheet nor its folder: the create waits for finalize.
    assert _named(writer, scratch, "never") == []


def test_a_sheets_failure_after_the_create_leaves_the_sheet_named_and_reusable(
        writer, scratch, sql, gdrive_secret_sql):
    run = _run_name(writer, scratch)
    target = f"gdrive://{fx.SCRATCH_ROOT}/{run}/made_then_failed"
    # Sheets refuses a tab title over 100 characters (400) -- the first
    # batchUpdate, sent right after the create.
    long_tab = "t" * 101
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT 1 AS a) TO '{target}' (FORMAT sheet_range, RANGE '{long_tab}!A1');""",
              expect_error=True)
    made = _named(writer, scratch, "made_then_failed")
    assert len(made) == 1, made
    sheet = made[0]["id"]
    assert "created a new Sheet" in out and sheet in out and "left in place empty" in out, out
    assert "100 characters" in out, out
    # Left in place, not trashed -- also by DuckDB's failed-COPY cleanup.
    assert _drive_state(writer, sheet) == {"id": sheet, "trashed": False, "mimeType": SHEET}

    # A re-run with a valid title writes INTO it: same id, still one Sheet.
    sql(f"""{gdrive_secret_sql}
        COPY (SELECT 1 AS a) TO '{target}' (FORMAT sheet_range, RANGE 'Fixed!A1');""")
    assert [m["id"] for m in _named(writer, scratch, "made_then_failed")] == [sheet]
    assert _values(writer, sheet, "Fixed!A1:A2") == [["a"], ["1"]]
