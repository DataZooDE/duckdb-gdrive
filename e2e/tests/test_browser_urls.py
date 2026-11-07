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


DRIVE_V3 = "https://www.googleapis.com/drive/v3"


def _meta(drive, file_id: str) -> dict:
    resp = drive._request("GET", f"{DRIVE_V3}/files/{file_id}", "probe.get",
                          params={"fields": "id,trashed,name"})
    return {"status": resp.status_code, **(resp.json() if resp.ok else {})}


@pytest.mark.parametrize("permanent", [False, True])
def test_a_failed_copy_to_an_address_leaves_the_file_alone(writer, scratch, sql, gdrive_secret_sql, permanent):
    """COPY's failure cleanup removes "the file it created" -- the tmp path,
    which for an address maps to the user's own file. It used to trash it,
    or with gdrive_permanent_delete delete it for good (found in review)."""
    file_id = writer.upload("keep.csv", b"a\n1\n", scratch, "text/csv")
    url = f"https://drive.google.com/file/d/{file_id}/view"
    setting = "SET gdrive_permanent_delete = true;" if permanent else ""
    out = sql(f"""{gdrive_secret_sql}
        {setting}
        COPY (SELECT CASE WHEN i < 3 THEN i ELSE error('boom') END AS a FROM range(5) t(i)) TO '{url}' (FORMAT csv);""",
              expect_error=True)
    assert "boom" in out, out
    assert _meta(writer, file_id) == {"status": 200, "id": file_id, "trashed": False, "name": "keep.csv"}
    assert writer.download(file_id) == b"a\n1\n"


def test_a_versioned_address_reads_that_version_and_is_read_only(writer, scratch, sql, gdrive_secret_sql):
    file_id = writer.upload("v.csv", b"a\n1\n", scratch, "text/csv")
    writer.update_content(file_id, b"a\n2\n")
    resp = writer._request("GET", f"{DRIVE_V3}/files/{file_id}/revisions", "revisions.list",
                           params={"fields": "revisions(id)"})
    old = resp.json()["revisions"][0]["id"]
    url = f"https://drive.google.com/file/d/{file_id}/view?usp=sharing&version={old}"
    out = sql(f"""{gdrive_secret_sql}
        SELECT a FROM read_csv('{url}');""")
    assert out.splitlines()[-1] == "1", out
    # remove_file and move_file of "a version" would act on the current file.
    for stmt in (f"SELECT remove_file('{url}');",
                 f"SELECT move_file('{url}', 'gdrive://scratch/{writer.get_metadata(scratch)['name']}/moved.csv');"):
        out = sql(f"{gdrive_secret_sql}\n{stmt}", expect_error=True)
        assert "read-only" in out, out
    assert _meta(writer, file_id)["trashed"] is False and _meta(writer, file_id)["name"] == "v.csv"


def test_writes_need_the_file_address_form(writer, scratch, sql, gdrive_secret_sql):
    file_id = writer.upload("w.csv", b"a\n1\n", scratch, "text/csv")
    folder = writer.create_folder("dest", scratch)
    for target in (f"https://drive.google.com/file/d/{file_id}",
                   f"https://drive.google.com/open?id={file_id}",
                   f"https://drive.google.com/drive/folders/{folder}"):
        out = sql(f"""{gdrive_secret_sql}
            COPY (SELECT 1 AS a) TO '{target}' (FORMAT csv);""", expect_error=True)
        assert "write through a file's own address" in out, (target, out)
    assert writer.download(file_id) == b"a\n1\n"
    assert writer.list_children(folder) == []


def test_a_folder_address_is_exactly_the_folder(sql, gdrive_secret_sql, fixtures_root, drive):
    parts = drive.resolve_path("parts", root_id=fixtures_root)
    out = sql(f"""{gdrive_secret_sql}
        SELECT count(*) FROM read_gdrive_files('https://drive.google.com/drive/folders/{parts}/*.parquet');""",
              expect_error=True)
    assert "names no file or folder" in out, out
