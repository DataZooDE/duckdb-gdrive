"""gdrive_changes: what changed under a folder since a token, end to end.

In the e2e layer because the changes feed LAGS: a file uploaded a few seconds
ago is often not in changes.list yet (observed 2026-11-07: 3 s was not
enough). A SQLLogicTest cannot wait for it; this polls, with a deadline.

The last test is the mirror pattern from the README -- a listing, then the
changes since a token applied to it -- checked against a fresh listing. That
is the property the function exists for: a mirror that follows the feed ends
up equal to the folder.
"""

from __future__ import annotations

import time

import pytest

pytestmark = pytest.mark.live

#: How long to wait for Drive's changes feed to show a change.
FEED_DEADLINE_S = 180


def _rows(out: str) -> list[list[str]]:
    # CREATE SECRET prints a bare "true"; result rows are '|'-separated.
    return [line.split("|") for line in out.splitlines() if "|" in line]


def _changes(sql, secret, folder: str, token: str | None) -> list[list[str]]:
    arg = "" if token is None else f", token := '{token}'"
    return _rows(sql(f"""{secret}
        SELECT _change, coalesce(id, ''), coalesce(path, ''), coalesce(is_folder::VARCHAR, ''), coalesce(token, '')
        FROM gdrive_changes('{folder}'{arg});"""))


def _token(rows: list[list[str]]) -> str:
    checkpoints = [r for r in rows if r[0] == "checkpoint"]
    assert len(checkpoints) == 1, rows
    assert rows[-1][0] == "checkpoint", "the checkpoint row comes last"
    assert checkpoints[0][4], "the checkpoint carries the token"
    return checkpoints[0][4]


def _poll(sql, secret, folder: str, token: str, done) -> list[list[str]]:
    deadline = time.monotonic() + FEED_DEADLINE_S
    while True:
        rows = _changes(sql, secret, folder, token)
        if done(rows):
            return rows
        if time.monotonic() > deadline:
            raise AssertionError(f"the changes feed did not show the change within {FEED_DEADLINE_S}s:\n{rows}")
        time.sleep(10)


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
    outside = writer.create_folder("out", scratch)
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))

    sub = writer.create_folder("sub", inside)
    new = writer.upload("new.csv", b"a\n2\n", sub, "text/csv")
    elsewhere = writer.upload("elsewhere.csv", b"a\n2\n", outside, "text/csv")
    writer.delete(doomed, permanent=False)  # trashed: a delete, still with its path

    def done(rows):
        ids = {r[1] for r in rows}
        return {new, doomed, sub} <= ids

    rows = _poll(sql, gdrive_secret_sql, base, token, done)
    by_id = {r[1]: r for r in rows if r[0] != "checkpoint"}
    assert by_id[new][:4] == ["upsert", new, f"{base}/sub/new.csv", "false"]
    assert by_id[sub][:4] == ["upsert", sub, f"{base}/sub", "true"]
    assert by_id[doomed][:3] == ["delete", doomed, f"{base}/doomed.csv"]
    # Outside the folder: a delete by id with no path (it may have moved out);
    # it may not be in the feed yet, so only its shape is checked.
    if elsewhere in by_id:
        assert by_id[elsewhere][0] == "delete" and by_id[elsewhere][2] == "", by_id[elsewhere]
    # The watched folder itself never appears.
    assert inside not in by_id
    new_token = _token(rows)

    # From the new token, nothing we made is reported again.
    again = _changes(sql, gdrive_secret_sql, base, new_token)
    assert not ({new, sub, doomed} & {r[1] for r in again}), again


def test_a_mirror_that_follows_the_feed_equals_the_folder(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}/m"
    folder = writer.create_folder("m", scratch)
    a = writer.upload("a.csv", b"x\n1\n", folder, "text/csv")
    b = writer.upload("b.csv", b"x\n1\n", folder, "text/csv")

    # The README's order: the token FIRST, then the listing.
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    mirror = {r[1]: r[2] for r in _changes(sql, gdrive_secret_sql, base, None) if r[0] == "upsert" and r[3] == "false"}
    assert set(mirror) == {a, b}

    writer.delete(a, permanent=True)
    c = writer.upload("c.csv", b"x\n1\n", folder, "text/csv")
    writer.update_content(b, b"x\n2\n")

    rows = _poll(sql, gdrive_secret_sql, base, token, lambda rs: {a, c} <= {r[1] for r in rs})
    for r in rows:
        if r[0] == "checkpoint":
            continue
        mirror.pop(r[1], None)
        if r[0] == "upsert" and r[3] == "false":
            mirror[r[1]] = r[2]

    fresh = _rows(sql(f"""{gdrive_secret_sql}
        SELECT id, path FROM read_gdrive_files('{base}');"""))
    assert mirror == {i: p for i, p in fresh}
