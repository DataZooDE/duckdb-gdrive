"""Errors say what to fix, and writes through aliases stay safe.

Found by exploratory testing (2026-11-07). Each case reproduces the mistake a
tester made and asserts on the message, or on what Drive holds afterwards.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live

DRIVE_V3 = "https://www.googleapis.com/drive/v3"
SA = ("CREATE SECRET s (TYPE gdrive, PROVIDER service_account, KEY_FILE getenv('GDRIVE_CI_SA_KEY_FILE'), "
      "{binding}{scope});")


def _sa(binding: str, scope: str = "") -> str:
    return SA.format(binding=binding, scope=f", DRIVE_SCOPE '{scope}'" if scope else "")


def test_user_mistakes_carry_no_report_it_hint(sql, gdrive_secret_sql):
    out = sql(f"""{gdrive_secret_sql}
        SELECT * FROM read_gdrive_files(NULL);""", expect_error=True)
    assert "must not be NULL" in out and "Unexpected?" not in out, out
    out = sql(f"""{gdrive_secret_sql}
        SELECT * FROM gdrive_changes('gdrive://fixtures', token := 'not-a-token');""", expect_error=True)
    assert "does not accept the token" in out and "Unexpected?" not in out, out


def test_a_folder_id_as_drive_id_says_use_root_folder_id(sql):
    out = sql(_sa("DRIVE_ID getenv('GDRIVE_CI_DRIVE_ID')") + "\nSELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');",
              expect_error=True)
    assert "DRIVE_ID takes a Shared Drive id" in out and "ROOT_FOLDER_ID" in out, out


def test_a_missing_root_folder_says_so(sql):
    out = sql(_sa("ROOT_FOLDER_ID 'nonexistentfolder123'") + "\nSELECT * FROM read_gdrive_files('gdrive://fixtures/small.csv');",
              expect_error=True)
    assert "ROOT_FOLDER_ID does not exist" in out, out


def test_a_read_only_scope_write_names_drive_scope(sql, scratch, writer):
    name = writer.get_metadata(scratch)["name"]
    out = sql(_sa("ROOT_FOLDER_ID getenv('GDRIVE_CI_DRIVE_ID')")
              + f"\nCOPY (SELECT 1 AS a) TO 'gdrive://scratch/{name}/ro.csv';", expect_error=True)
    assert "read-only" in out and "DRIVE_SCOPE" in out, out


def test_an_invalid_scope_names_drive_scope(sql):
    out = sql(_sa("ROOT_FOLDER_ID getenv('GDRIVE_CI_DRIVE_ID')", scope="drive")
              + "\nSELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');", expect_error=True)
    assert "DRIVE_SCOPE is a valid OAuth scope" in out, out


def test_a_bad_refresh_token_says_sign_in_again(sql):
    out = sql("""CREATE SECRET r (TYPE gdrive, PROVIDER config, CLIENT_ID getenv('GDRIVE_OAUTH_CLIENT_ID'),
                 CLIENT_SECRET getenv('GDRIVE_OAUTH_CLIENT_SECRET'), REFRESH_TOKEN 'not-a-refresh-token',
                 ROOT_FOLDER_ID getenv('GDRIVE_CI_DRIVE_ID'));
              SELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');""", expect_error=True)
    assert "Sign in again" in out and "make oauth_consent" not in out, out


def test_doc_exports_have_no_bom_and_honour_the_format(sql, gdrive_secret_sql):
    out = sql(f"""{gdrive_secret_sql}
        SELECT hex(content[1:3]) FROM read_text('gdrive://fixtures/Notes');""")
    assert out.splitlines()[-1] != "EFBBBF", out
    out = sql(f"""{gdrive_secret_sql}
        SET gdrive_docs_export_mime = 'application/pdf';
        SELECT decode(content[1:4]) FROM read_blob('gdrive://fixtures/Notes');""")
    assert out.splitlines()[-1] == "%PDF", out


def test_copy_to_an_id_path_writes_in_place(writer, scratch, sql, gdrive_secret_sql):
    """COPY's default tmp staging used to fail on an id: target
    ('gdrive:/tmp_id:<id>'), and a failed COPY must not remove the file."""
    file_id = writer.upload("t.csv", b"a\n1\n", scratch, "text/csv")
    sql(f"""{gdrive_secret_sql}
        COPY (SELECT 7 AS a) TO 'gdrive://id:{file_id}' (FORMAT csv);""")
    assert writer.download(file_id) == b"a\n7\n"
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT CASE WHEN i < 2 THEN i ELSE error('boom') END AS a FROM range(4) t(i))
        TO 'gdrive://id:{file_id}' (FORMAT csv);""", expect_error=True)
    assert "boom" in out, out
    resp = writer._request("GET", f"{DRIVE_V3}/files/{file_id}", "probe.get", params={"fields": "trashed"})
    assert resp.json() == {"trashed": False}
    assert writer.download(file_id) == b"a\n7\n"
    names = sorted(c["name"] for c in writer.list_children(scratch) if c["name"] != ".heartbeat")
    assert names == ["t.csv"], names


def test_a_trailing_slash_target_is_a_folder(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    out = sql(f"""{gdrive_secret_sql}
        COPY (SELECT 1 AS a) TO 'gdrive://scratch/{name}/out/' (FORMAT csv);""", expect_error=True)
    assert "names a folder, not a file" in out, out
    assert [c["name"] for c in writer.list_children(scratch) if c["name"] != ".heartbeat"] == []
