"""O-1: the glob cache must not smuggle a file past the R-4 ambiguity check.

`Glob` caches the children it listed so DuckDB's multi-file reader does not
re-resolve each leaf (151 listings to read 150 files, before). The duplicate
guard originally counted exact relative paths -- which is not enough.

With a recursive `**`, two folders sharing a name each contribute their OWN
children, and those children's relative paths are individually unique:

    dup             <- appears twice, correctly not cached
    dup/only_a.csv  <- appears once
    dup/only_b.csv  <- appears once

So both leaves got cached. A later open of `dup/only_a.csv` then hit the
full-path entry and never walked the `dup` segment -- so it never raised the
ambiguity error that names both folder ids. A loud, correct failure became a
silent choice between two Drive branches, decided by listing order.

Found by review, not by a test. This is that test.

Needs two folders with the same name in one parent, which only an external
client can create -- the extension refuses to. Hence e2e rather than
SQLLogicTest.
"""

from __future__ import annotations

import subprocess

import pytest

pytestmark = pytest.mark.live


def _scratch_name(drive, scratch_id):
    return drive.get_metadata(scratch_id)["name"]


def _run(duckdb_cli, secret_sql, statements):
    """Run statements in one process; return combined output."""
    script = secret_sql + "\n" + "\n".join(statements) + "\n"
    p = subprocess.run([str(duckdb_cli), "-noheader", "-list"], input=script,
                       capture_output=True, text=True, timeout=300)
    return p.stdout + p.stderr


def test_glob_must_not_cache_leaves_under_an_ambiguous_folder(
        writer, scratch, duckdb_cli, gdrive_secret_sql):
    name = _scratch_name(writer, scratch)
    # Two folders, same name, same parent. Drive permits this; it is R-4.
    f1 = writer.create_folder("dup", scratch)
    f2 = writer.create_folder("dup", scratch)
    assert f1 != f2
    writer.upload("only_a.csv", b"a\n1\n", f1, "text/csv")
    writer.upload("only_b.csv", b"a\n2\n", f2, "text/csv")

    out = _run(duckdb_cli, gdrive_secret_sql, [
        # Recursive glob: this is what populates the cache with the leaves.
        f"SELECT 'globbed=' || count(*) FROM "
        f"glob('gdrive://scratch/{name}/**/*.csv');",
        # Now address a UNIQUELY named leaf whose ANCESTOR is ambiguous. The
        # path is not resolvable -- `dup` names two folders -- so this must
        # raise, naming both folder ids, whether or not a glob ran first.
        f"SELECT count(*) FROM read_csv('gdrive://scratch/{name}/dup/only_a.csv');",
    ])

    assert "globbed=" in out, f"the glob itself failed:\n{out}"
    assert "ambiguous" in out.lower(), (
        "reading a leaf under a duplicated folder name must raise the R-4 "
        "ambiguity error. Getting a row count here means the glob cached the "
        "leaf and the open never walked the ambiguous segment -- results now "
        "depend on Drive's listing order.\n\n" + out
    )
    # The error must name both folder ids, so the user can pick one with the
    # id: form. "ambiguous" alone is not actionable.
    assert f1 in out and f2 in out, (
        f"the ambiguity error must name both folder ids ({f1}, {f2}):\n{out}"
    )
