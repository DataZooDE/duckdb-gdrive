"""S-0.6 -- prove the harness itself works before any C++ depends on it.

This is the slice that establishes credentials are real, the Shared Drive is
reachable and writable, scratch lifecycle works, and the permanent fixtures
are present and shaped as the later waves assume.

If these fail, nothing downstream is trustworthy, so they are deliberately
blunt and assert concrete values.
"""

from __future__ import annotations

import pytest

from helpers import fixtures as fx
from helpers.drive import FOLDER_MIME

pytestmark = pytest.mark.live


def test_credentials_mint_a_token_and_reach_drive(drive):
    """The service account authenticates and the Shared Drive is visible."""
    children = drive.list_children(drive.drive_id)
    names = {c["name"] for c in children}
    assert fx.FIXTURES_ROOT in names, (
        f"Shared Drive {drive.drive_id} has no /{fx.FIXTURES_ROOT}; "
        "run `make seed_fixtures`"
    )


def test_scratch_roundtrip(drive, scratch):
    """Upload, read back byte-for-byte, and range-read -- inside a scratch folder.

    This is the exact capability the extension's read path needs, exercised
    through the harness first so a later failure is unambiguously the C++.
    """
    payload = b"col\n" + b"".join(f"{i}\n".encode() for i in range(100))
    file_id = drive.upload("roundtrip.csv", payload, scratch, "text/csv")

    assert drive.download(file_id) == payload

    # Drive honours HTTP Range -- the premise the whole read path rests on.
    first10 = drive.download(file_id, byte_range=(0, 9))
    assert first10 == payload[:10], f"ranged read returned {first10!r}"

    meta = drive.get_metadata(file_id)
    assert int(meta["size"]) == len(payload)
    assert meta["headRevisionId"], "no headRevisionId -- GetVersionTag would have nothing to return"


def test_scratch_is_cleaned_up(drive):
    """Teardown really deletes: a leaked folder per run would fill the Drive."""
    scratch_root = drive.find_or_create_folder(fx.SCRATCH_ROOT)
    before = {c["id"] for c in drive.list_children(scratch_root)}

    import uuid
    name = f"run-{uuid.uuid4().hex[:12]}"
    fid = drive.create_folder(name, scratch_root)
    assert fid in {c["id"] for c in drive.list_children(scratch_root)}
    drive.delete(fid, permanent=True)

    after = {c["id"] for c in drive.list_children(scratch_root)}
    assert after == before


def test_permanent_fixtures_present(drive, fixtures_root):
    """The fixture tree matches what later waves assume."""
    top = {c["name"]: c for c in drive.list_children(fixtures_root)}
    for expected in ["small.csv", "empty.csv", "nested", "dup", "parts", "many", "Budget", "Notes"]:
        assert expected in top, f"missing fixture {expected!r}; run `make seed_fixtures`"

    assert top["nested"]["mimeType"] == FOLDER_MIME
    assert top["Budget"]["mimeType"] == fx.SHEET_MIME
    assert top["Notes"]["mimeType"] == fx.DOC_MIME


def test_duplicate_name_fixture_really_is_duplicated(drive, fixtures_root):
    """R-4 needs two files with one name in one folder. Prove Drive allows it.

    If this ever stops being true the collision-handling code is dead weight
    and the plan's R-4 mitigation needs revisiting -- so assert it explicitly
    rather than assuming.
    """
    dup_dir = drive.resolve_path("dup", root_id=fixtures_root)
    dups = drive.list_children(dup_dir, name="dup.csv")
    assert len(dups) == 2, f"expected exactly 2 files named dup.csv, found {len(dups)}"
    assert dups[0]["id"] != dups[1]["id"]


def test_pagination_fixture_exceeds_one_page(drive, fixtures_root):
    """>100 children, so a single-page ListFiles cannot pass by accident."""
    many = drive.resolve_path("many", root_id=fixtures_root)
    children = drive.list_children(many)
    assert len(children) == fx.PAGINATION_COUNT, (
        f"expected {fx.PAGINATION_COUNT} children, got {len(children)} -- "
        "either seeding is incomplete or pagination is broken in the harness"
    )


def test_native_sheet_has_no_bytes_but_exports(drive, fixtures_root):
    """REQ-F-07's premise: alt=media fails on a native file, export works.

    This is the behavioural fork the extension has to implement, verified
    against real Drive before any code assumes it.
    """
    sheet_id = drive.resolve_path("Budget", root_id=fixtures_root)

    with pytest.raises(RuntimeError) as excinfo:
        drive.download(sheet_id)
    assert "403" in str(excinfo.value) or "fileNotDownloadable" in str(excinfo.value)

    exported = drive.export(sheet_id, "text/csv")
    assert b"quarter" in exported and b"revenue" in exported


def test_resolve_path_walks_segments(drive, fixtures_root):
    """Path resolution costs one files.list per segment -- the R-1 premise."""
    drive.reset_call_counts()
    file_id = drive.resolve_path("nested/a/b/deep.csv", root_id=fixtures_root)
    assert file_id
    # 4 segments -> 4 lookups. This number is the thing the extension's cache
    # exists to reduce, and the baseline the R-1 assertions compare against.
    assert drive.calls.get("files.list") == 4, drive.calls
