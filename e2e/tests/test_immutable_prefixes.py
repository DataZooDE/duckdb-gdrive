"""`gdrive_immutable_prefixes` -- the saving, and the price.

The setting skips OpenFile's per-open metadata refresh for paths the user
declares are never overwritten in place. That refresh costs ~270 ms per file
per query, and for a lakehouse data path it can never find anything, because
DuckLake/Iceberg/Delta write new files instead of rewriting old ones.

The price is exact and worth pinning down in a test rather than only in prose:
if you declare a prefix whose files ARE rewritten in place, reads go stale and
NOTHING DETECTS IT. Drive keeps a file's id across an overwrite, so there is
no 404; and Drive returns no ETag and ignores If-Match, so there is no cheap
validation either (both verified against the live API 2026-08-01).

So there are two tests here and they are equally important:

  * default off  -> an external in-place overwrite IS picked up
  * declared on  -> the same overwrite is NOT picked up, within a process

The second asserts a documented weakness rather than a feature. Writing it the
other way round -- asserting the stale read cannot happen -- would be asserting
something the implementation deliberately does not provide, and would fail the
moment the optimisation actually worked.

Needs a mutation from OUTSIDE the duckdb process (so the extension's own
invalidation never runs) between two reads INSIDE one process. SQLLogicTest
cannot express that; hence the e2e layer.
"""

from __future__ import annotations

import subprocess

import pytest

pytestmark = pytest.mark.live

ROWS_BEFORE = b"a\n1\n"
ROWS_AFTER = b"a\n1\n2\n3\n"


def _session(duckdb_cli, secret_sql, extra_setup=""):
    proc = subprocess.Popen(
        [str(duckdb_cli), "-noheader", "-list"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )
    assert proc.stdin and proc.stdout
    proc.stdin.write(secret_sql + "\n")
    if extra_setup:
        proc.stdin.write(extra_setup + "\n")
    proc.stdin.flush()
    return proc


def _count(proc, path, marker):
    proc.stdin.write(f"SELECT '{marker}=' || count(*) FROM read_csv('{path}');\n")
    proc.stdin.flush()
    return _read_until(proc.stdout, f"{marker}=")


def _scratch_name(drive, scratch_id):
    return drive.get_metadata(scratch_id)["name"]


def test_overwrite_is_seen_when_prefixes_not_declared(
        writer, scratch, duckdb_cli, gdrive_secret_sql):
    """Default behaviour: the per-open refresh still runs, so a rewrite lands."""
    name = _scratch_name(writer, scratch)
    path = f"gdrive://scratch/{name}/mutable.csv"
    file_id = writer.upload("mutable.csv", ROWS_BEFORE, scratch, "text/csv")

    proc = _session(duckdb_cli, gdrive_secret_sql)
    try:
        assert _count(proc, path, "warm") == "warm=1"

        # In place: same id, new bytes. No 404 is possible here.
        writer.update_content(file_id, ROWS_AFTER, "text/csv")
        assert writer.get_metadata(file_id)["id"] == file_id, "id must survive"

        assert _count(proc, path, "after") == "after=3", (
            "with the optimisation off, a second query must re-check metadata "
            "and observe the rewritten file"
        )
    finally:
        proc.stdin.close()
        proc.wait(timeout=60)


def test_overwrite_is_missed_when_prefix_declared_immutable(
        writer, scratch, duckdb_cli, gdrive_secret_sql):
    """The documented price. Declaring a MUTABLE path immutable goes stale."""
    name = _scratch_name(writer, scratch)
    path = f"gdrive://scratch/{name}/declared.csv"
    file_id = writer.upload("declared.csv", ROWS_BEFORE, scratch, "text/csv")

    proc = _session(
        duckdb_cli, gdrive_secret_sql,
        f"SET gdrive_immutable_prefixes='gdrive://scratch/{name}';",
    )
    try:
        assert _count(proc, path, "warm") == "warm=1"

        writer.update_content(file_id, ROWS_AFTER, "text/csv")

        after = _count(proc, path, "after")
        assert after == "after=1", (
            f"expected the STALE row count (1), got {after!r}. If this now "
            "reports 3, the extension has gained a way to detect an in-place "
            "overwrite -- which would be good news, but it means this setting's "
            "documented trade-off, the README warning and the paper's Section 4 "
            "all need revisiting rather than this assertion being 'fixed'."
        )
    finally:
        proc.stdin.close()
        proc.wait(timeout=60)


def test_declaring_a_sibling_prefix_does_not_exempt(
        writer, scratch, duckdb_cli, gdrive_secret_sql):
    """Prefix matching is on segments: 'scratch/run-x' must not cover
    'scratch/run-xyz'. A character-wise starts_with would, and would silently
    disable freshness checks for a directory the user never named."""
    name = _scratch_name(writer, scratch)
    path = f"gdrive://scratch/{name}/sibling.csv"
    file_id = writer.upload("sibling.csv", ROWS_BEFORE, scratch, "text/csv")

    # A prefix that is a strict character-prefix of the real one but NOT a
    # path-segment prefix of it.
    truncated = f"gdrive://scratch/{name[:-1]}"
    assert truncated != f"gdrive://scratch/{name}"

    proc = _session(duckdb_cli, gdrive_secret_sql,
                    f"SET gdrive_immutable_prefixes='{truncated}';")
    try:
        assert _count(proc, path, "warm") == "warm=1"
        writer.update_content(file_id, ROWS_AFTER, "text/csv")
        assert _count(proc, path, "after") == "after=3", (
            "a non-segment prefix must NOT suppress the freshness check"
        )
    finally:
        proc.stdin.close()
        proc.wait(timeout=60)


def _read_until(stream, marker: str, limit: int = 200) -> str:
    for _ in range(limit):
        line = stream.readline()
        if not line:
            break
        line = line.strip()
        if line.startswith(marker):
            return line
        if "Error" in line:
            raise AssertionError(f"duckdb error before {marker!r}: {line}")
    raise AssertionError(f"never saw {marker!r} in duckdb output")


def test_deleted_file_reaches_the_new_id_on_the_block_cache_read_path(
        writer, scratch, duckdb_cli, gdrive_secret_sql):
    """A dead file id must be re-resolved at READ time on the DEFAULT read path.

    Regression guard for a bug found in review. Skipping the open-time metadata
    refresh removed one of the two places a dead id was caught, and the comment
    justifying that claimed read-time recovery covered the rest. It did not:
    recovery was wired only into the exact-range fallback, reachable solely with
    the block cache switched off. On the default 16 MiB blocks a 404 inside the
    block fetch threw straight out.

    WHAT THIS CAN AND CANNOT RESTORE. Recovery re-resolves the path and reads
    the NEW file's bytes -- visible in a GDRIVE_TRACE_FILE trace as
    404 -> files.list -> 206. It cannot undo the SIZE already reported to
    DuckDB at open, which came from the stale metadata, so a replacement of a
    different length is read short. Recovery turns a hard failure into a
    bounded read; it is not a substitute for the open-time refresh, and the
    contract in the next test is what keeps the situation from arising.

    `gdrive_block_cache_bytes=0` clears cached blocks, which is what forces the
    read to reach Drive at all -- with blocks retained nothing is asked and
    nothing is learned (see the next test).
    """
    name = _scratch_name(writer, scratch)
    path = f"gdrive://scratch/{name}/recreated.csv"
    first_id = writer.upload("recreated.csv", ROWS_BEFORE, scratch, "text/csv")

    proc = _session(
        duckdb_cli, gdrive_secret_sql,
        f"SET gdrive_immutable_prefixes='gdrive://scratch/{name}';\n"
        "SET gdrive_block_cache_bytes=0;",
    )
    try:
        assert _count(proc, path, "warm") == "warm=1"

        writer.delete(first_id, permanent=True)
        # Deliberately the SAME length as ROWS_BEFORE. A dead id must not be a
        # hard error, and with an equal-length replacement the stale size does
        # not truncate, so the recovered read is exactly correct. The
        # differing-length case is described in the docstring rather than
        # asserted: its outcome is a property of the stale size, not recovery.
        replacement = b"a\n9\n"
        assert len(replacement) == len(ROWS_BEFORE)
        second_id = writer.upload("recreated.csv", replacement, scratch, "text/csv")
        assert second_id != first_id, "Drive reused the id; this proves nothing"

        after = _count(proc, path, "after")
        assert after == "after=1", (
            f"expected a successful recovered read, got {after!r}. An error "
            "here means the 404 on the block path was not caught and the "
            "handle was never re-resolved."
        )
        proc.stdin.write(f"SELECT 'val=' || max(a) FROM read_csv('{path}');\n")
        proc.stdin.flush()
        assert _read_until(proc.stdout, "val=") == "val=9", (
            "recovered the read but served the OLD content"
        )
    finally:
        proc.stdin.close()
        proc.wait(timeout=60)


def test_delete_is_invisible_while_blocks_are_cached(
        writer, scratch, duckdb_cli, gdrive_secret_sql):
    """The full extent of what `gdrive_immutable_prefixes` gives up.

    With the refresh skipped AND the file's blocks already cached, a second
    read issues no Drive request at all -- so a delete-and-recreate is as
    invisible as an in-place overwrite. Recovery cannot help: there is no 404
    to recover from, because nothing is asked.

    So the setting's contract is stronger than "not overwritten in place". It
    is: not modified, not replaced, and not deleted-and-recreated at this path
    while the process runs. That holds for a lakehouse DATA_PATH, whose files
    are written once under a UUID name and deleted only once no live snapshot
    references them. Pinned by a test so the contract is not prose alone.
    """
    name = _scratch_name(writer, scratch)
    path = f"gdrive://scratch/{name}/cached.csv"
    first_id = writer.upload("cached.csv", ROWS_BEFORE, scratch, "text/csv")

    proc = _session(
        duckdb_cli, gdrive_secret_sql,
        f"SET gdrive_immutable_prefixes='gdrive://scratch/{name}';",
    )
    try:
        assert _count(proc, path, "warm") == "warm=1"
        writer.delete(first_id, permanent=True)
        writer.upload("cached.csv", ROWS_AFTER, scratch, "text/csv")

        after = _count(proc, path, "after")
        assert after == "after=1", (
            f"expected the STALE cached content (1 row), got {after!r}. If this "
            "reports 3, the extension detects a delete under an immutable "
            "prefix after all -- good news, but narrow the documented contract "
            "in the README and the paper to match rather than 'fixing' this."
        )
    finally:
        proc.stdin.close()
        proc.wait(timeout=60)
