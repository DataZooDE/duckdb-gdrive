"""D-6: deletion trashes by default, and `gdrive_permanent_delete` opts out.

The README promises this:

    `DELETE` moves to **trash**, not permanent deletion --
    `SET gdrive_permanent_delete=true` to opt in.

It was untested. Worse, the SQL suite carried a block *titled* "RemoveFile
trashes by default" which created a file called `doomed.csv`, asserted it
existed, and never deleted anything. A test named for a behaviour it does not
exercise is worse than no test: it makes the coverage report lie.

Two reasons this lives in the e2e layer rather than SQLLogicTest:

* There is no SQL function that removes a file. RemoveFile is reached through
  DuckDB internals -- here, by OVERWRITING an existing path, which resolves
  the old file and removes it before the new one takes its place.
* "Trashed" versus "gone" is invisible from SQL. Both look like absence. Only
  Drive's own metadata distinguishes them, which needs the harness.

The distinction matters because D-6 chose recoverability deliberately: a
table-format cleanup routine hard-deleting the wrong thing in someone's own
Drive is not recoverable, and Drive's trash is.
"""

from __future__ import annotations

import subprocess

import pytest

pytestmark = pytest.mark.live

DRIVE_V3 = "https://www.googleapis.com/drive/v3"


def _file_state(drive, file_id: str) -> str:
    """'trashed', 'live', or 'gone' -- straight from Drive."""
    resp = drive._request("GET", f"{DRIVE_V3}/files/{file_id}", "probe.get",
                          params={"fields": "id,trashed"})
    if resp.status_code == 404:
        return "gone"
    resp.raise_for_status()
    return "trashed" if resp.json().get("trashed") else "live"


def _overwrite(duckdb_cli, secret_sql: str, path: str, extra_setup: str = "") -> None:
    sql = f"{secret_sql}\n{extra_setup}\nCOPY (SELECT 42 AS a) TO '{path}' (FORMAT csv);"
    proc = subprocess.run([str(duckdb_cli), "-noheader", "-list", "-c", sql],
                          capture_output=True, text=True, timeout=300)
    if proc.returncode != 0:
        raise AssertionError(f"overwrite failed:\n{proc.stdout}{proc.stderr}")


def test_overwrite_trashes_the_old_file_by_default(
    writer, scratch, duckdb_cli, gdrive_secret_sql
):
    name = writer.get_metadata(scratch)["name"]
    old_id = writer.upload("victim.csv", b"a\n1\n", scratch, "text/csv")
    assert _file_state(writer, old_id) == "live"

    _overwrite(duckdb_cli, gdrive_secret_sql, f"gdrive://scratch/{name}/victim.csv")

    assert _file_state(writer, old_id) == "trashed", (
        "the replaced file should be recoverable from Drive's trash (D-6)"
    )
    # ...and the path now resolves to a DIFFERENT, live file.
    current = [c for c in writer.list_children(scratch) if c["name"] == "victim.csv"]
    assert len(current) == 1, f"expected exactly one victim.csv, got {len(current)}"
    assert current[0]["id"] != old_id


def test_permanent_delete_setting_really_hard_deletes(
    writer, scratch, duckdb_cli, gdrive_secret_sql
):
    name = writer.get_metadata(scratch)["name"]
    old_id = writer.upload("victim2.csv", b"a\n1\n", scratch, "text/csv")
    assert _file_state(writer, old_id) == "live"

    _overwrite(duckdb_cli, gdrive_secret_sql, f"gdrive://scratch/{name}/victim2.csv",
               extra_setup="SET gdrive_permanent_delete=true;")

    # Not merely trashed: unrecoverable. This is the opt-in, and it must be
    # observably different from the default or the setting does nothing.
    assert _file_state(writer, old_id) == "gone", (
        "gdrive_permanent_delete=true must hard-delete, not trash -- "
        "otherwise the setting is decorative"
    )
