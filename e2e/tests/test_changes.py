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

    # From the new token, what comes back again must agree with what we already
    # have. Drive's feed is AT LEAST once: one move can yield a second entry
    # that lands after the token we were given (seen in CI, 2026-11-07: the
    # moved-in file again, as the same upsert). Re-applying it is harmless for
    # a mirror; a DIFFERENT answer for the same item would not be.
    again = _by_id(_changes(sql, gdrive_secret_sql, base, _token(rows)))
    for item in wanted & set(again):
        assert again[item][:3] == by_id[item][:3], (again[item], by_id[item])


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


def _feed_parents(drive, token: str) -> dict[str, list[str]]:
    """file id -> parents, for every change since `token`: what changes.list
    returns for this identity, read raw. A removed entry, or one with no file
    record, maps to [] -- like a file without parents, gdrive_changes confirms
    it with one files.get before it may become a delete."""
    out: dict[str, list[str]] = {}
    page = token
    while page:
        resp = drive._request("GET", f"{DRIVE_V3}/changes", "changes.list", params={
            "pageToken": page, "pageSize": "1000", "includeRemoved": "true",
            "fields": "nextPageToken,changes(fileId,removed,file(parents))"})
        resp.raise_for_status()
        data = resp.json()
        for c in data.get("changes", []):
            if not c.get("removed") and "file" in c:
                out[c["fileId"]] = c["file"].get("parents", [])
            else:
                out[c["fileId"]] = []
        page = data.get("nextPageToken")
    return out


def _feed_parents_with(drive, token: str, ids) -> dict[str, list[str]]:
    """_feed_parents, polled until it holds every id in `ids`. Drive's change
    feed is read from replicas that lag each other: gdrive_changes saw all
    twenty files and the very next raw read missed one (CI, 2026-11-13)."""
    deadline = time.monotonic() + FEED_DEADLINE_S
    while True:
        feed = _feed_parents(drive, token)
        if set(ids) <= set(feed) or time.monotonic() > deadline:
            return feed
        time.sleep(5)


def _ancestor_folders(drive, starts, stop: str, seen: set[str]) -> None:
    """Every folder above `starts`, up to `stop` or the top, into `seen`: the
    folders a parent walk may have to look up (one files.get each)."""
    todo = [p for p in starts if p != stop]
    while todo:
        fid = todo.pop()
        if fid in seen or fid == stop:
            continue
        seen.add(fid)
        resp = drive._request("GET", f"{DRIVE_V3}/files/{fid}", "files.get", params={"fields": "parents"})
        if resp.status_code == 200:
            todo.extend(resp.json().get("parents", []))


def test_since_token_cost_is_bounded_by_folders_not_files(writer, scratch, sql, gdrive_secret_sql):
    """One files.get per folder the parent walk has not seen; a sibling
    listing per distinct parent (for duplicate names) -- not one call per file.

    Drive's feed is per identity, not per folder: every change the delegated
    user can see -- other CI runs, the nightly sweep, developers, parallel
    agents, the user's own Drive -- is in the window and costs its own parent
    walk. A fixed bound over that flakes (seen 2026-11-13: exactly 12 against
    `< 12`). So the bound is computed from the window itself: our folder, plus
    every folder above anything ELSE the feed reported. Twenty files in one
    folder add one folder to that and twenty items to the feed; a call per
    file would overshoot by about nineteen whatever anyone else is doing.
    """
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}"
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    sub = writer.create_folder("many", scratch)
    ids = [writer.upload(f"f{i}.csv", b"a\n1\n", sub, "text/csv") for i in range(20)]
    _poll(sql, gdrive_secret_sql, base, token, lambda rs: set(ids) <= set(_by_id(rs)))

    # The feed, read raw BEFORE and AFTER the measured call and merged: a
    # foreign item deleted in between is "removed" afterwards (no parents), and
    # one created in between is only in the second read. What remains missed is
    # an item both created and deleted within the call itself -- seconds.
    before = _feed_parents_with(writer, token, ids)
    # One process: the first gdrive_changes resolves the path and warms the path
    # cache, so the second's counters are the feed walk alone.
    out = _rows(sql(f"""{gdrive_secret_sql}
        SELECT count(*) FROM gdrive_changes('{base}', token := 'latest');
        CALL gdrive_reset_stats();
        SELECT count(*) FROM gdrive_changes('{base}', token := '{token}');
        SELECT metric || '|' || value FROM gdrive_stats() WHERE metric IN ('files_get', 'files_list');"""))
    after = _feed_parents_with(writer, token, ids)
    stats = {r[0]: int(r[1]) for r in out if r[0] in ("files_get", "files_list")}
    feed = {**before, **after}
    assert set(ids) <= set(feed), "the raw feed shows our files (the poll above saw them)"

    # Our own subtree costs exactly one lookup: `many`, whose parent is the
    # watched folder. Everything else in the window is someone else's.
    # (The watched folder itself is in the feed too -- adding children touches
    # it -- but gdrive_changes skips it without a lookup.)
    foreign: set[str] = set()
    ours = set(ids) | {sub, scratch}
    _ancestor_folders(writer, {p for fid, ps in feed.items() if fid not in ours for p in ps}, scratch, foreign)
    foreign.discard(sub)
    # Plus one confirming files.get per entry the walk cannot place (removed,
    # or no parents) -- in EITHER read, since the call may have seen either.
    confirms = {fid for fid, ps in [*before.items(), *after.items()] if not ps and fid not in ours}
    bound = 1 + len(foreign) + len(confirms)
    # +2 for what the merged reads can still miss (a foreign folder created and
    # deleted inside the call). Measured 2026-11-13 with a release live suite
    # writing alongside: files_get 10 against a computed bound of exactly 10;
    # quiet runs, 1. A call per file would be 20 over.
    assert stats["files_get"] <= bound + 2, (
        stats, f"bound {bound} = 1 + {len(foreign)} foreign folder(s) + {len(confirms)} confirm(s)")
    # And the bound means something: a call per file would need the 20 own
    # files on top of whatever the others cost.
    assert bound < len(feed), (bound, len(feed))

    # Listings never touch foreign changes (with outside := false they are only
    # placed, never walked), so their bound is ours alone: one sibling listing
    # each for `many`'s parent and the files' parent, one subtree walk of
    # `many`. Fixed, and a call per file would be 20+.
    assert stats["files_list"] <= 3, stats


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
    seen = [r for r in rows if r[0] != "checkpoint" and r[1] in (a, b, c, sub, s1, leaving, l1)]
    for r in rows:
        if r[0] == "checkpoint":
            continue
        mirror.pop(r[1], None)
        if r[0] == "upsert" and r[3] == "false":
            mirror[r[1]] = r[2]

    fresh = _rows(sql(f"""{gdrive_secret_sql}
        SELECT id, path FROM read_gdrive_files('{base}/**');"""))
    assert mirror == {i: p for i, p in fresh}, {"rows for our ids, in feed order": seen,
                                                 "c": c, "a": a, "b": b, "sub": sub, "leaving": leaving}
