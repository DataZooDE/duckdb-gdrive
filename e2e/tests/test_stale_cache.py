"""A cached file id must not outlive the file it points at.

Codex review #2, finding 3. The path-resolution cache maps a path to a Drive
file id. If ANOTHER client deletes that file and creates a new one at the
same path, Drive assigns a NEW id -- the cached one is dead. Before the fix,
OpenFile refreshed metadata by the cached id, ignored the 404, and opened a
handle onto the dead id; every read then failed "not found" for a path that
plainly existed, until the process restarted.

Expressing this needs three things at once: a warm cache inside a LIVE duckdb
process, a mutation performed OUTSIDE that process (so our own invalidation
does not run), and a second read from the same process. SQLLogicTest cannot
do the middle step, which is exactly why the e2e layer exists.
"""

from __future__ import annotations

import subprocess

import pytest

pytestmark = pytest.mark.live


def test_cached_id_survives_external_delete_and_recreate(writer, scratch, duckdb_cli, gdrive_secret_sql):
    path = f"gdrive://scratch/{scratch_name(writer, scratch)}/stale.csv"
    # Seeded by the harness, not by DuckDB, so DuckDB learns the id only by
    # resolving the path.
    first_id = writer.upload("stale.csv", b"a\n1\n", scratch, "text/csv")

    proc = subprocess.Popen(
        [str(duckdb_cli), "-noheader", "-list"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )
    assert proc.stdin and proc.stdout

    # 1. Warm the cache: this resolves the path and stores first_id.
    proc.stdin.write(gdrive_secret_sql + "\n")
    proc.stdin.write(f"SELECT 'warm=' || count(*) FROM read_csv('{path}');\n")
    proc.stdin.flush()
    warm = _read_until(proc.stdout, "warm=")
    assert warm == "warm=1", warm

    # 2. External client replaces the file. Same path, NEW id -- the cached
    #    one now points at nothing.
    writer.delete(first_id, permanent=True)
    second_id = writer.upload("stale.csv", b"a\n1\n2\n", scratch, "text/csv")
    assert second_id != first_id, "Drive reused the id; this test would prove nothing"

    # 3. Same process, same warm cache. Must notice the id is dead, re-resolve
    #    the path, and read the NEW file.
    proc.stdin.write(f"SELECT 'after=' || count(*) FROM read_csv('{path}');\n")
    proc.stdin.flush()
    after = _read_until(proc.stdout, "after=")
    proc.stdin.close()
    proc.wait(timeout=60)

    assert after == "after=2", (
        f"expected the re-resolved file (2 rows), got {after!r}. "
        "A stale cached id was used."
    )


def scratch_name(drive, scratch_id: str) -> str:
    return drive.get_metadata(scratch_id)["name"]


def _read_until(stream, marker: str, limit: int = 200) -> str:
    """Read lines until one starts with `marker`; return it."""
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
