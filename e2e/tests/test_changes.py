"""gdrive_changes: what changed under a folder since a token, end to end.

In the e2e layer because the changes feed LAGS: a file uploaded a few seconds
ago is often not in changes.list yet (observed 2026-11-07: from ~3.5 s to
several minutes). A SQLLogicTest cannot wait for it; this polls, with a
deadline.

Moves are where a per-drive feed meets a per-folder function, so most tests
here move things: in, out, a folder renamed, a folder moved out. The last
test is the README's mirror pattern -- a listing, then the changes since a
token applied to it -- checked against a fresh listing after every kind of
change, including the folder moves exploratory testing found it diverged on.
"""

from __future__ import annotations

import time

import pytest

pytestmark = pytest.mark.live

#: How long to wait for Drive's changes feed to show a change.
FEED_DEADLINE_S = 420
DRIVE_V3 = "https://www.googleapis.com/drive/v3"


def _rows(out: str) -> list[list[str]]:
    # CREATE SECRET prints a bare "true"; result rows are '|'-separated.
    return [line.split("|") for line in out.splitlines() if "|" in line]


def _changes(sql, secret, folder: str, token: str | None, outside: bool = False) -> list[list[str]]:
    args = "" if token is None else f", token := '{token}'"
    if outside:
        args += ", outside := true"
    return _rows(sql(f"""{secret}
        SELECT _change, coalesce(id, ''), coalesce(path, ''), coalesce(is_folder::VARCHAR, ''), coalesce(token, '')
        FROM gdrive_changes('{folder}'{args});"""))


def _token(rows: list[list[str]]) -> str:
    checkpoints = [r for r in rows if r[0] == "checkpoint"]
    assert len(checkpoints) == 1, rows
    assert rows[-1][0] == "checkpoint", "the checkpoint row comes last"
    assert checkpoints[0][4], "the checkpoint carries the token"
    return checkpoints[0][4]


def _poll(sql, secret, folder: str, token: str, done, outside: bool = False) -> list[list[str]]:
    deadline = time.monotonic() + FEED_DEADLINE_S
    while True:
        rows = _changes(sql, secret, folder, token, outside)
        if done(rows):
            return rows
        if time.monotonic() > deadline:
            raise AssertionError(f"the changes feed did not show the change within {FEED_DEADLINE_S}s:\n{rows}")
        time.sleep(10)


def _move(drive, file_id: str, new_parent: str, old_parent: str) -> None:
    resp = drive._request("PATCH", f"{DRIVE_V3}/files/{file_id}", "files.update",
                          params={"addParents": new_parent, "removeParents": old_parent, "fields": "id"})
    resp.raise_for_status()


def _rename(drive, file_id: str, name: str) -> None:
    resp = drive._request("PATCH", f"{DRIVE_V3}/files/{file_id}", "files.update", json={"name": name},
                          params={"fields": "id"})
    resp.raise_for_status()


def _by_id(rows):
    return {r[1]: r for r in rows if r[0] != "checkpoint"}


def test_without_a_token_lists_everything_once(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    inside = writer.create_folder("in", scratch)
    writer.upload("keep.csv", b"a\n1\n", inside, "text/csv")
    sub = writer.create_folder("sub", inside)
    writer.upload("deep.csv", b"a\n1\n", sub, "text/csv")
    outside = writer.create_folder("out", scratch)
    writer.upload("other.csv", b"a\n1\n", outside, "text/csv")

    rows = _changes(sql, gdrive_secret_sql, f"gdrive://scratch/{name}/in", None)
    _token(rows)
    upserts = sorted((r[2], r[3]) for r in rows if r[0] == "upsert")
    base = f"gdrive://scratch/{name}/in"
    assert upserts == [(f"{base}/keep.csv", "false"), (f"{base}/sub", "true"), (f"{base}/sub/deep.csv", "false")]
    assert not [r for r in rows if r[0] == "delete"]


def test_duplicate_names_get_id_paths_that_read_back(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}"
    a = writer.upload("twin.csv", b"x\n1\n", scratch, "text/csv")
    b = writer.upload("twin.csv", b"x\n2\n", scratch, "text/csv")
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    rows = _changes(sql, gdrive_secret_sql, base, None)
    by_id = _by_id(rows)
    assert by_id[a][2] == f"gdrive://id:{a}" and by_id[b][2] == f"gdrive://id:{b}", rows
    # ...and the since-token rows too, once the second twin is in the feed.
    c = writer.upload("twin.csv", b"x\n3\n", scratch, "text/csv")
    rows = _poll(sql, gdrive_secret_sql, base, token, lambda rs: c in _by_id(rs))
    assert _by_id(rows)[c][2] == f"gdrive://id:{c}", rows
    out = sql(f"""{gdrive_secret_sql}
        SELECT x FROM read_csv('gdrive://id:{c}');""")
    assert out.splitlines()[-1] == "3", out


def test_latest_is_only_a_token(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    writer.upload("x.csv", b"a\n1\n", scratch, "text/csv")
    rows = _changes(sql, gdrive_secret_sql, f"gdrive://scratch/{name}", "latest")
    assert len(rows) == 1 and rows[0][0] == "checkpoint" and rows[0][4], rows


def test_changes_since_a_token(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}/in"
    inside = writer.create_folder("in", scratch)
    doomed = writer.upload("doomed.csv", b"a\n1\n", inside, "text/csv")
    leaving = writer.upload("leaving.csv", b"a\n1\n", inside, "text/csv")
    outside = writer.create_folder("out", scratch)
    arriving = writer.upload("arriving.csv", b"a\n1\n", outside, "text/csv")
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))

    sub = writer.create_folder("sub", inside)
    new = writer.upload("new.csv", b"a\n2\n", sub, "text/csv")
    elsewhere = writer.upload("elsewhere.csv", b"a\n2\n", outside, "text/csv")
    writer.delete(doomed, permanent=False)  # trashed: a delete, still with its path
    _move(writer, leaving, outside, inside)  # moved OUT
    _move(writer, arriving, inside, outside)  # moved IN

    wanted = {new, doomed, sub, arriving}
    rows = _poll(sql, gdrive_secret_sql, base, token, lambda rs: wanted <= set(_by_id(rs)))
    by_id = _by_id(rows)
    assert by_id[new][:4] == ["upsert", new, f"{base}/sub/new.csv", "false"]
    assert by_id[sub][:4] == ["upsert", sub, f"{base}/sub", "true"]
    assert by_id[arriving][:3] == ["upsert", arriving, f"{base}/arriving.csv"]
    assert by_id[doomed][:3] == ["delete", doomed, f"{base}/doomed.csv"]
    # By default, what is no longer (or never was) under the folder is not
    # reported: Drive's feed cannot tell a move out from a change elsewhere.
    assert leaving not in by_id and elsewhere not in by_id, rows
    assert inside not in by_id  # the watched folder itself never appears
    assert len({r[1] for r in rows if r[0] != "checkpoint"}) == len([r for r in rows if r[0] != "checkpoint"]), \
        "one row per id"

    # outside := true reports them, as path-less deletes -- what a mirror needs.
    rows = _poll(sql, gdrive_secret_sql, base, token, lambda rs: {leaving, elsewhere} <= set(_by_id(rs)),
                 outside=True)
    by_id = _by_id(rows)
    assert by_id[leaving][:3] == ["delete", leaving, ""], by_id[leaving]
    assert by_id[elsewhere][:3] == ["delete", elsewhere, ""], by_id[elsewhere]

    # From the new token, nothing we made is reported again.
    again = _changes(sql, gdrive_secret_sql, base, _token(rows))
    assert not (wanted & set(_by_id(again))), again


def test_a_renamed_folder_brings_its_subtree(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}"
    folder = writer.create_folder("fold", scratch)
    child = writer.upload("in1.csv", b"a\n1\n", folder, "text/csv")
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    _rename(writer, folder, "fold2")
    rows = _poll(sql, gdrive_secret_sql, base, token, lambda rs: folder in _by_id(rs))
    by_id = _by_id(rows)
    assert by_id[folder][:3] == ["upsert", folder, f"{base}/fold2"]
    # The feed reports only the folder; its child's path changed with it.
    assert by_id[child][:3] == ["upsert", child, f"{base}/fold2/in1.csv"], rows


def test_a_folder_moved_out_takes_its_subtree(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}/src"
    src = writer.create_folder("src", scratch)
    away = writer.create_folder("away", scratch)
    folder = writer.create_folder("fold", src)
    child = writer.upload("in1.csv", b"a\n1\n", folder, "text/csv")
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    _move(writer, folder, away, src)
    rows = _poll(sql, gdrive_secret_sql, base, token, lambda rs: folder in _by_id(rs), outside=True)
    by_id = _by_id(rows)
    assert by_id[folder][0] == "delete" and by_id[child][0] == "delete", rows


def test_since_token_cost_is_bounded_by_folders_not_files(writer, scratch, sql, gdrive_secret_sql):
    """One files.get per folder the parent walk has not seen; a sibling
    listing per distinct parent (for duplicate names) -- not one call per file."""
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}"
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    sub = writer.create_folder("many", scratch)
    ids = [writer.upload(f"f{i}.csv", b"a\n1\n", sub, "text/csv") for i in range(20)]
    _poll(sql, gdrive_secret_sql, base, token, lambda rs: set(ids) <= set(_by_id(rs)))
    out = _rows(sql(f"""{gdrive_secret_sql}
        CALL gdrive_reset_stats();
        SELECT count(*) FROM gdrive_changes('{base}', token := '{token}');
        SELECT metric || '|' || value FROM gdrive_stats() WHERE metric IN ('files_get', 'files_list');"""))
    stats = {r[0]: int(r[1]) for r in out}
    # 20 new files in one new folder: a parent walk of a couple of gets, a
    # sibling listing per distinct parent, the folder's subtree walk. Other
    # activity the test account can see also costs walks, so the bound is
    # loose -- but 20+ would mean a call per file.
    assert stats["files_get"] < 12 and stats["files_list"] < 12, stats


def test_a_mirror_that_follows_the_feed_equals_the_folder(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}/m"
    folder = writer.create_folder("m", scratch)
    away = writer.create_folder("away", scratch)
    a = writer.upload("a.csv", b"x\n1\n", folder, "text/csv")
    b = writer.upload("b.csv", b"x\n1\n", folder, "text/csv")
    sub = writer.create_folder("sub", folder)
    s1 = writer.upload("s1.csv", b"x\n1\n", sub, "text/csv")
    leaving = writer.create_folder("leaving", folder)
    l1 = writer.upload("l1.csv", b"x\n1\n", leaving, "text/csv")

    # The README's order: the token FIRST, then the listing.
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    mirror = {r[1]: r[2] for r in _changes(sql, gdrive_secret_sql, base, None) if r[0] == "upsert" and r[3] == "false"}
    assert set(mirror) == {a, b, s1, l1}

    writer.delete(a, permanent=True)
    c = writer.upload("c.csv", b"x\n1\n", folder, "text/csv")
    writer.update_content(b, b"x\n2\n")
    _rename(writer, sub, "sub2")             # its child's path changes
    _move(writer, leaving, away, folder)     # its child leaves too

    def done(rs):
        got = _by_id(rs)
        return {a, c, sub, leaving} <= set(got)

    rows = _poll(sql, gdrive_secret_sql, base, token, done, outside=True)
    for r in rows:
        if r[0] == "checkpoint":
            continue
        mirror.pop(r[1], None)
        if r[0] == "upsert" and r[3] == "false":
            mirror[r[1]] = r[2]

    fresh = _rows(sql(f"""{gdrive_secret_sql}
        SELECT id, path FROM read_gdrive_files('{base}/**');"""))
    assert mirror == {i: p for i, p in fresh}
