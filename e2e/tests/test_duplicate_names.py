"""A write must not manufacture a duplicate name, and a write to a name that
already has byte-identical duplicates repairs it.

The incident (heron-escurel, 2026-11-18): escurel wrote a brand-new
skills/agent.md twice, about 3 s apart. Before each write the extension asks
Drive whether the name exists (a files.list `name=` query) and creates when
nothing matches. That query is eventually consistent, so the second write did
not see the file the first had created seconds earlier and created another.
Two identical agent.md then made the path ambiguous (R-4), and the gateway
died on every boot from 2026-11-20 on.

`test_writing_a_new_name_twice_back_to_back_leaves_one_file` is the property
that broke. It is statistical by nature -- listing lag cannot be forced
against real Drive (D-1 rules out a fake) -- so it writes many fresh names,
each twice in one process, and requires exactly one file for every one.

The healing tests are deterministic: the duplicate pair is made directly with
the Drive API, the way it exists in heron's lane.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live

NAMES = 12


def _named(writer, folder: str, name: str) -> list[dict]:
    return [c for c in writer.list_children(folder) if c["name"] == name]


def test_writing_a_new_name_twice_back_to_back_leaves_one_file(writer, scratch, sql, gdrive_secret_sql):
    folder = writer.get_metadata(scratch)["name"]
    stmts = [gdrive_secret_sql]
    for i in range(NAMES):
        path = f"gdrive://scratch/{folder}/twice-{i:02d}.md"
        # The second write is the one that used to create a second file.
        stmts.append(f"SELECT write_blob('{path}', 'first {i}'::BLOB);")
        stmts.append(f"SELECT write_blob('{path}', 'second {i}'::BLOB);")
    sql("\n".join(stmts))

    children = writer.list_children(scratch)
    for i in range(NAMES):
        name = f"twice-{i:02d}.md"
        named = [c for c in children if c["name"] == name]
        assert len(named) == 1, (
            f"{name}: {len(named)} files after two writes in one process -- the second write did not "
            "see the file the first had just created (Drive's name listing lags) and created another"
        )
    # And the second write is the one that stuck.
    out = sql(gdrive_secret_sql + f"\nSELECT content FROM read_text('gdrive://scratch/{folder}/twice-00.md');")
    assert out.splitlines()[-1] == "second 0"


def test_a_write_to_an_identical_duplicate_keeps_the_oldest_and_trashes_the_rest(
    writer, scratch, sql, gdrive_secret_sql
):
    folder = writer.get_metadata(scratch)["name"]
    first = writer.upload("agent.md", b"same bytes\n", scratch, "text/markdown")
    second = writer.upload("agent.md", b"same bytes\n", scratch, "text/markdown")
    assert len(_named(writer, scratch, "agent.md")) == 2, "the fixture pair did not get made"

    sql(gdrive_secret_sql + f"\nSELECT write_blob('gdrive://scratch/{folder}/agent.md', 'new bytes\n'::BLOB);")

    left = _named(writer, scratch, "agent.md")
    assert [c["id"] for c in left] == [first], (
        f"expected only the oldest copy {first} to remain, found {[c['id'] for c in left]}"
    )
    trashed = writer.get_metadata(second, fields="id,trashed")
    assert trashed["trashed"] is True, "the newer copy must be trashed (recoverable), not deleted (D-6)"
    out = sql(gdrive_secret_sql + f"\nSELECT content FROM read_text('gdrive://scratch/{folder}/agent.md');")
    assert out.splitlines()[-1] == "new bytes"


def test_a_write_to_differing_duplicates_still_refuses(writer, scratch, sql, gdrive_secret_sql):
    folder = writer.get_metadata(scratch)["name"]
    writer.upload("dup.md", b"one\n", scratch, "text/markdown")
    writer.upload("dup.md", b"two\n", scratch, "text/markdown")

    out = sql(gdrive_secret_sql + f"\nSELECT write_blob('gdrive://scratch/{folder}/dup.md', 'x'::BLOB);",
              expect_error=True)
    assert "is ambiguous" in out
    assert len(_named(writer, scratch, "dup.md")) == 2, "differing copies must never be touched"


def test_healing_can_be_switched_off(writer, scratch, sql, gdrive_secret_sql):
    folder = writer.get_metadata(scratch)["name"]
    writer.upload("same.md", b"same\n", scratch, "text/markdown")
    writer.upload("same.md", b"same\n", scratch, "text/markdown")

    out = sql(gdrive_secret_sql + "\nSET gdrive_heal_identical_duplicates = false;"
              f"\nSELECT write_blob('gdrive://scratch/{folder}/same.md', 'x'::BLOB);", expect_error=True)
    assert "is ambiguous" in out
    assert len(_named(writer, scratch, "same.md")) == 2


def test_a_write_right_after_our_own_create_asks_drive_by_id(writer, scratch, sql, gdrive_secret_sql):
    """The deterministic half of the property above.

    Listing lag cannot be forced, but what the extension does about it can be
    observed: the second write of a name this process just created must look
    that file up by id (files.get, strongly consistent) rather than rely on
    the name listing alone.
    """
    folder = writer.get_metadata(scratch)["name"]
    path = f"gdrive://scratch/{folder}/mine.md"
    out = sql(gdrive_secret_sql + f"""
        SELECT write_blob('{path}', 'one'::BLOB);
        SELECT count(*) FROM gdrive_reset_stats();
        SELECT write_blob('{path}', 'two'::BLOB);
        SELECT 'stat|' || metric || '|' || value FROM gdrive_stats() WHERE metric IN ('files_get', 'files_create');""")
    stats = {line.split("|")[1]: int(line.split("|")[2]) for line in out.splitlines() if line.startswith("stat|")}
    assert stats.get("files_get", 0) >= 1, f"the second write never asked for its own file by id: {stats}"
    assert stats.get("files_create", 0) == 0, f"the second write created a file: {stats}"
