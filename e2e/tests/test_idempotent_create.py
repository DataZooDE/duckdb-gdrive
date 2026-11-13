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
import requests

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


# ---------------------------------------------------------------------------
# 2026-11-13: every write the extension sends is retry-safe. These pin the
# Drive behaviour each rule in IsRetrySafe / InterpretCreateReply
# (src/gdrive_upload_plan.cpp) rests on -- the same request sent twice, as a
# retry after a lost answer would send it. There is no lost-reply hook in the
# extension (D-1); the property is Drive's, so it is tested against Drive.
# ---------------------------------------------------------------------------

FOLDER_MIME = "application/vnd.google-apps.folder"


def _reserve(drive, count: int = 1) -> list[str]:
    resp = drive._request("GET", f"{DRIVE_V3}/files/generateIds", "generateIds",
                          params={"count": count, "space": "drive"})
    resp.raise_for_status()
    ids = resp.json()["ids"]
    assert len(ids) == count, ids
    return ids


def _create_folder_with_id(drive, parent_id: str, name: str, folder_id: str):
    # The exact request the extension sends for a folder: plain files.create,
    # JSON metadata only, carrying the reserved id.
    return drive._request("POST", f"{DRIVE_V3}/files", "create.folder.reserved",
                          params={"fields": "id,name,mimeType"},
                          json={"id": folder_id, "name": name, "mimeType": FOLDER_MIME,
                                "parents": [parent_id]})


def _reason(resp) -> str:
    return resp.json()["error"]["errors"][0]["reason"]


def test_a_reserved_id_makes_a_repeated_folder_create_a_conflict_not_a_duplicate(writer, scratch):
    """The spike behind reserving ids for folders: Drive accepts a supplied id
    on a folder create, and a repeat is 409 fileIdInUse -- the reason the
    extension requires before adopting the id as its own."""
    (reserved,) = _reserve(writer)
    first = _create_folder_with_id(writer, scratch, "retried-dir", reserved)
    assert first.status_code == 200, first.text[:300]
    assert first.json()["id"] == reserved, "Drive ignored the reserved id on a folder"
    assert first.json()["mimeType"] == FOLDER_MIME

    second = _create_folder_with_id(writer, scratch, "retried-dir", reserved)
    assert second.status_code == 409, second.text[:300]
    assert _reason(second) == "fileIdInUse", second.text[:300]

    named = [c for c in writer.list_children(scratch) if c["name"] == "retried-dir"]
    assert [c["id"] for c in named] == [reserved]


def test_one_reservation_covers_a_whole_new_chain(writer, scratch):
    """generateIds?count=N, as the extension asks for a write into a new
    subtree: N distinct ids, each usable for its own create, folder or file."""
    ids = _reserve(writer, 3)
    assert len(set(ids)) == 3
    parent = scratch
    for name, fid in zip(("a", "b"), ids[:2]):
        r = _create_folder_with_id(writer, parent, name, fid)
        assert r.status_code == 200 and r.json()["id"] == fid, r.text[:300]
        parent = fid
    r = _create_with_id(writer, parent, "leaf.csv", ids[2])
    assert r.status_code == 200 and r.json()["id"] == ids[2], r.text[:300]


def test_a_resumable_create_publishes_under_its_reserved_id_once(writer, scratch):
    """A streaming create's session carries the reserved id, so an ambiguous
    FINAL chunk can be looked up by it: nothing exists under the id until the
    final chunk lands, and a second session with the same id cannot publish a
    second file."""
    (reserved,) = _reserve(writer)

    def start():
        r = writer._request("POST", f"{UPLOAD_V3}/files", "resumable.init",
                            params={"uploadType": "resumable", "fields": "id,name,size"},
                            json={"id": reserved, "name": "streamed.bin", "parents": [scratch]},
                            headers={"X-Upload-Content-Type": "application/octet-stream"})
        assert r.status_code == 200, r.text[:300]
        return r.headers["Location"]

    def final(session):
        # The session URI is its own credential: no bearer, as in the extension.
        return requests.put(session, data=b"x" * 100, headers={"Content-Range": "bytes 0-99/100"},
                            timeout=60)

    session = start()
    before = writer._request("GET", f"{DRIVE_V3}/files/{reserved}", "probe.get", params={"fields": "id"})
    assert before.status_code == 404, "a session published before its final chunk"

    done = final(session)
    assert done.status_code in (200, 201), done.text[:300]
    assert done.json()["id"] == reserved

    again = final(start())
    assert again.status_code == 409, again.text[:300]
    assert _reason(again) == "fileIdInUse"

    named = [c for c in writer.list_children(scratch) if c["name"] == "streamed.bin"]
    assert [c["id"] for c in named] == [reserved]


def test_a_repeated_overwrite_leaves_one_file_with_the_content(writer, scratch):
    """An overwrite is the same bytes on the same id: sent twice it is still
    one file holding them -- which is why its PATCH is retried."""
    file_id = writer.upload("over.csv", b"a\n1\n", scratch, "text/csv")

    def overwrite():
        files = {"metadata": ("metadata.json", json.dumps({"name": "over.csv"}), "application/json"),
                 "file": ("over.csv", b"a\n2\n", "text/csv")}
        r = writer._request("PATCH", f"{UPLOAD_V3}/files/{file_id}", "update.multipart",
                            params={"uploadType": "multipart", "fields": "id"}, files=files)
        assert r.status_code == 200 and r.json()["id"] == file_id, r.text[:300]

    overwrite()
    overwrite()
    named = [c for c in writer.list_children(scratch) if c["name"] == "over.csv"]
    assert [c["id"] for c in named] == [file_id]
    assert writer.download(file_id) == b"a\n2\n"


def test_a_repeated_move_answers_with_the_same_state(writer, scratch):
    """move_file's PATCH (name + addParents + removeParents) sent twice: the
    repeat is a 200 with the file exactly where the first put it, so a retry
    after a lost answer is safe -- and the state read back by id is what the
    extension reconciles against when every answer was lost (MoveLanded)."""
    a = writer.create_folder("a", scratch)
    b = writer.create_folder("b", scratch)
    file_id = writer.upload("x.csv", b"a\n1\n", a, "text/csv")
    for _ in range(2):
        r = writer._request("PATCH", f"{DRIVE_V3}/files/{file_id}", "files.update.move",
                            params={"fields": "id,name,parents", "addParents": b, "removeParents": a},
                            json={"name": "y.csv"})
        assert r.status_code == 200, r.text[:300]
        assert r.json()["name"] == "y.csv" and r.json()["parents"] == [b], r.text[:300]
    assert writer.list_children(a) == []
    assert [c["id"] for c in writer.list_children(b)] == [file_id]


def test_a_repeated_trash_answers_with_the_same_state(writer, scratch):
    """Trash is {trashed: true}: absolute, so the repeat is a 200 too."""
    file_id = writer.upload("t.csv", b"a\n1\n", scratch, "text/csv")
    for _ in range(2):
        r = writer._request("PATCH", f"{DRIVE_V3}/files/{file_id}", "files.update.trash",
                            params={"fields": "id,trashed"}, json={"trashed": True})
        assert r.status_code == 200 and r.json()["trashed"] is True, r.text[:300]
