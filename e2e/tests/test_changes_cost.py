"""gdrive_changes' confirm-before-delete: what it costs, per entry.

An entry the feed gives no way to place -- `removed`, no file record, no
parents -- is checked with ONE files.get before it may become a delete
(Drive's feed lags; a live file can arrive in that shape). An entry with
parents is placed as before, at no extra cost. This pins both numbers.

Its own file because it is a cost assertion (CLAUDE.md), and in the e2e layer
because it asserts on changes SINCE a token, which means polling a lagging
feed.

Drive's feed is per identity, not per folder: every change the delegated user
can see -- other CI runs, the nightly sweep, developers -- lands in the same
window and costs its own lookups, so no exact number can be read off one call.
And a token split around our own writes does not isolate them either: tried
first, and in 24 samples over four splits our entries never sat between the
tokens taken just before and just after the writes (2026-11-13).
So both sides are bounded instead, from the call's own rows and the raw feed:

- FLOOR. Without outside := true, a path-less delete row comes only from a
  confirm (Drive said 404, or the file is not under the folder), so the call
  spent at least one files.get per such row -- ours among them. The code
  before the confirm emitted the same rows for free.
- CEILING. One files.get per entry the walk cannot place (removed or without
  parents, in either raw read), plus one per foreign folder above anything
  else in the window, plus our own subtree's one. Our uploads add nothing:
  a confirm per placed entry would overshoot by N_NEW whatever anyone else
  is doing.
"""

from __future__ import annotations

import pytest

from .test_changes import _ancestor_folders, _by_id, _changes, _feed_parents_with, _poll, _rows, _token

pytestmark = pytest.mark.live

N_GONE = 8
N_NEW = 8


@pytest.mark.timeout(900)  # the feed poll alone may take minutes
def test_a_confirm_costs_one_files_get_and_a_placed_entry_none(writer, scratch, sql, gdrive_secret_sql):
    name = writer.get_metadata(scratch)["name"]
    base = f"gdrive://scratch/{name}"
    sub = writer.create_folder("w", scratch)
    gone = [writer.upload(f"g{i}.csv", b"x\n1\n", sub, "text/csv") for i in range(N_GONE)]
    token = _token(_changes(sql, gdrive_secret_sql, base, "latest"))
    for fid in gone:
        writer.delete(fid, permanent=True)  # `removed: true`, no file record: confirmed
    new = [writer.upload(f"n{i}.csv", b"x\n1\n", sub, "text/csv") for i in range(N_NEW)]  # parents: placed
    ours = set(gone) | set(new)
    _poll(sql, gdrive_secret_sql, base, token, lambda rs: ours <= set(_by_id(rs)))

    # Raw feed before and after the measured call, merged (as in
    # test_since_token_cost_is_bounded_by_folders_not_files).
    before = _feed_parents_with(writer, token, ours)
    out = _rows(sql(f"""{gdrive_secret_sql}
        SELECT 'warm|' || count(*) FROM gdrive_changes('{base}', token := 'latest');
        CALL gdrive_reset_stats();
        CREATE TEMP TABLE c AS SELECT * FROM gdrive_changes('{base}', token := '{token}');
        SELECT 'row|' || _change || '|' || id || '|' || coalesce(path, '') FROM c WHERE _change <> 'checkpoint';
        SELECT 'stat|' || metric || '|' || value FROM gdrive_stats() WHERE metric = 'files_get';"""))
    after = _feed_parents_with(writer, token, ours)
    files_get = int(next(r[2] for r in out if r[0] == "stat"))
    rows = {r[2]: (r[1], r[3]) for r in out if r[0] == "row"}

    # Ours came out right: the deleted ones as path-less deletes, the new ones
    # as upserts with their paths.
    for fid in gone:
        assert rows.get(fid) == ("delete", ""), (fid, rows.get(fid))
    for fid in new:
        assert rows.get(fid, ("",))[0] == "upsert", (fid, rows.get(fid))

    # FLOOR: one confirm per path-less delete row (ours: N_GONE of them).
    pathless = [fid for fid, (kind, path) in rows.items() if kind == "delete" and not path]
    assert files_get >= len(pathless) >= N_GONE, (files_get, len(pathless))

    # CEILING: one per unplaceable entry, one per foreign folder, one for `w`.
    confirms = {fid for fid, ps in [*before.items(), *after.items()] if not ps}
    feed = {**before, **after}
    foreign: set[str] = set()
    _ancestor_folders(writer, {p for fid, ps in feed.items() if fid not in ours | {sub, scratch} for p in ps},
                      scratch, foreign)
    foreign.discard(sub)
    bound = 1 + len(foreign) + len(confirms)
    print(f"files_get {files_get}; floor {len(pathless)} path-less deletes; ceiling {bound} = 1 + "
          f"{len(foreign)} foreign folder(s) + {len(confirms)} confirm(s); feed {len(feed)} entries")
    # +2 as in the folder-cost test: what the merged reads can still miss.
    assert files_get <= bound + 2, (files_get, f"bound {bound} = 1 + {len(foreign)} foreign folder(s) "
                                               f"+ {len(confirms)} confirm(s), {N_GONE} of them ours")
    assert set(gone) <= confirms, "the raw feed shows our deletes as removed"
    assert not set(new) & confirms, "the raw feed shows our uploads with parents"
