"""A retried create must never produce a second file.

Drive lets two files share a name in one folder, and this extension treats
that as a hard error (R-4) -- so a duplicate does not merely waste space, it
makes the path permanently unaddressable. A create that gets retried after an
ambiguous transport failure is exactly how that happens.

The extension reserves a file id (files.generateIds) before creating, so a
repeat lands on 409 "A file already exists with the provided ID" instead of
making a second file.

That property was verified by hand once, and hand-verification is not a test.
It lives here rather than in Catch2 because the property is Drive's behaviour,
not our JSON parsing: test/cpp/test_client.cpp covers the parser and must not
be mistaken for covering this.
"""

from __future__ import annotations

import json

import pytest

pytestmark = pytest.mark.live

DRIVE_V3 = "https://www.googleapis.com/drive/v3"
UPLOAD_V3 = "https://www.googleapis.com/upload/drive/v3"


def _create_with_id(drive, folder_id: str, name: str, file_id: str):
    meta = {"id": file_id, "name": name, "parents": [folder_id]}
    files = {
        "metadata": ("metadata.json", json.dumps(meta), "application/json"),
        "file": (name, b"a\n1\n", "text/csv"),
    }
    return drive._request("POST", f"{UPLOAD_V3}/files", "create.reserved",
                          params={"uploadType": "multipart"}, files=files)


def test_a_reserved_id_makes_a_repeated_create_a_conflict_not_a_duplicate(writer, scratch):
    """The exact race a retry causes: the same create, twice."""
    resp = writer._request("GET", f"{DRIVE_V3}/files/generateIds", "generateIds",
                           params={"count": 1, "space": "drive"})
    resp.raise_for_status()
    reserved = resp.json()["ids"][0]

    first = _create_with_id(writer, scratch, "retried.csv", reserved)
    assert first.status_code == 200, first.text[:300]
    assert first.json()["id"] == reserved, "Drive ignored the reserved id"

    # The retry. Without a reservation this makes a SECOND retried.csv and the
    # path becomes ambiguous forever.
    second = _create_with_id(writer, scratch, "retried.csv", reserved)
    assert second.status_code == 409, (
        f"a repeated create with a reserved id must conflict, got "
        f"{second.status_code}: {second.text[:300]}"
    )

    named = [c for c in writer.list_children(scratch) if c["name"] == "retried.csv"]
    assert len(named) == 1, (
        f"expected exactly one retried.csv, found {len(named)} -- the duplicate "
        "this mechanism exists to prevent"
    )


def test_without_a_reserved_id_drive_really_does_duplicate(writer, scratch):
    """The control. Without this, the test above proves nothing.

    If Drive rejected same-named creates on its own, the reservation would be
    pointless and the test above would pass for the wrong reason. It does not:
    two plain creates make two files.
    """
    for _ in range(2):
        writer.upload("plain.csv", b"a\n1\n", scratch, "text/csv")

    named = [c for c in writer.list_children(scratch) if c["name"] == "plain.csv"]
    assert len(named) == 2, (
        f"expected Drive to permit 2 files named plain.csv, found {len(named)}. "
        "If Drive has started rejecting duplicates, the reserved-id mechanism "
        "and the whole R-4 design need revisiting."
    )
