"""gdrive_permissions: who can reach a file, as Drive lists it.

Here rather than in SQLLogicTest because these rows need permissions the
fixtures do not have. No SQL function creates one -- the extension only reads
permissions -- so the harness adds them to scratch files and the tests assert
what the extension then reports.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live

DRIVE_V3 = "https://www.googleapis.com/drive/v3"


def _add_permission(drive, file_id: str, body: dict) -> None:
    resp = drive._request("POST", f"{DRIVE_V3}/files/{file_id}/permissions", "permissions.create",
                          json=body, params={"sendNotificationEmail": "false"})
    if resp.status_code == 403:
        # A Workspace domain may forbid sharing outside it; that is policy,
        # not something this test can or should work around.
        pytest.skip(f"the domain refuses this permission: {resp.text[:200]}")
    resp.raise_for_status()


def test_a_grant_below_an_inherited_one_is_not_recorded(writer, drive, scratch, sql, gdrive_secret_sql):
    """Drive folds a file-level grant into an equal or higher inherited one.

    Verified 2026-11-07: granting `reader` (or `writer`) on a file to someone
    who already inherits `writer` from its folder answers 200 -- and the
    permission's details still list only the inherited writer. Nothing records
    the file-level grant, so gdrive_permissions correctly says `inherited`.
    Pinned because an audit expecting a `direct` row would go looking for a
    grant Drive dropped. (A direct grant above the inherited role -- the
    owner -- is asserted in gdrive_permissions.test.template.)

    The grantee is the CI service account, which already reaches the file as a
    writer through the folder, so the grant widens nobody's access. (A `link`
    row would need an anyone-with-the-link permission, i.e. a public file; its
    mapping is covered by test/cpp/test_rows.cpp instead.)
    """
    name = writer.get_metadata(scratch)["name"]
    file_id = writer.upload("granted.csv", b"a\n1\n", scratch, "text/csv")
    sa_email = drive._creds.service_account_email
    _add_permission(writer, file_id, {"type": "user", "role": "reader", "emailAddress": sa_email})

    out = sql(f"""{gdrive_secret_sql}
        SELECT role, granted_by, grantee_type, link_scope IS NULL, link_url IS NULL, file_id = '{file_id}'
        FROM gdrive_permissions('gdrive://scratch/{name}/granted.csv')
        WHERE grantee_email = '{sa_email}';""")
    rows = [line for line in out.splitlines() if "|" in line]  # CREATE SECRET prints a bare "true"
    assert rows == ["writer|inherited|user|true|true|true"], out


def test_every_row_names_its_file(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    a = writer.upload("a.csv", b"x\n1\n", scratch, "text/csv")
    b = writer.upload("b.csv", b"x\n2\n", scratch, "text/csv")
    out = sql(f"""{gdrive_secret_sql}
        SELECT count(DISTINCT file_id), bool_and(file_id IN ('{a}', '{b}')),
               bool_and(granted_by IN ('direct', 'inherited', 'link')), count(*) >= 2
        FROM gdrive_permissions('gdrive://scratch/{name}/*.csv');""")
    assert out.splitlines()[-1] == "2|true|true|true", out
