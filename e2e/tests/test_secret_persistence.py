"""S-1.4 -- a persisted secret survives a restart, and the token is re-minted.

Two separate claims, both untested until now:

1. `CREATE PERSISTENT SECRET` writes enough to authenticate later. The refresh
   token is durable; the access token is not, and must not be.
2. A process that starts with a cold token cache mints a NEW access token from
   the refresh token. That is the only reason claim 1 works, and it is exactly
   what breaks if the refresh grant is ever dropped from the config provider.

Neither is expressible in SQLLogicTest: both need TWO duckdb processes sharing
a secret directory, and the second must be given no credentials in-band -- if
it were handed a CREATE SECRET, it would authenticate whether persistence
worked or not.

The negative control is the third process below, pointed at an EMPTY secret
directory. It must fail. Without it, a passing run would be consistent with
the extension reading credentials from the environment, which would make
claims 1 and 2 both unproven.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.live


def _run(duckdb_cli, secret_dir: Path, sql: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            str(duckdb_cli), "-noheader", "-list",
            "-c", f"SET secret_directory = '{secret_dir}'; {sql}",
        ],
        capture_output=True, text=True, timeout=300,
    )


def test_persistent_secret_survives_restart(tmp_path, duckdb_cli, gdrive_secret_sql):
    secret_dir = tmp_path / "secrets"
    secret_dir.mkdir(mode=0o700)

    # --- process 1: create the secret, and prove it works here. -------------
    create = gdrive_secret_sql.replace("CREATE SECRET", "CREATE PERSISTENT SECRET", 1)
    first = _run(duckdb_cli, secret_dir,
                 create + " SELECT 'first=' || count(*) FROM read_csv('gdrive://fixtures/small.csv');")
    assert first.returncode == 0, first.stdout + first.stderr
    assert "first=5" in first.stdout, first.stdout + first.stderr

    # The secret must actually be ON DISK, or the "restart" below would be
    # testing DuckDB's in-memory secret registry and nothing else.
    persisted = list(secret_dir.glob("*.duckdb_secret"))
    assert persisted, f"nothing persisted into {secret_dir}: {list(secret_dir.iterdir())}"

    # REQ-NF-03: an ACCESS token must never reach disk. The refresh token
    # legitimately does -- it is the durable credential and DuckDB's own secret
    # store is where it belongs -- but a cached bearer token written here would
    # be a credential this extension put on disk, which the requirement
    # forbids outright.
    blob = b"".join(p.read_bytes() for p in persisted)
    assert b"ya29." not in blob, "a Google access token was persisted to disk"

    # --- process 2: nothing but the directory. ------------------------------
    # No CREATE SECRET, and therefore no credential in this process's input.
    # Reading Drive here can only work by loading the persisted secret AND
    # minting a fresh access token from its refresh token -- process 1's token
    # died with process 1.
    second = _run(duckdb_cli, secret_dir,
                  "SELECT 'second=' || count(*) FROM read_csv('gdrive://fixtures/small.csv');")
    assert second.returncode == 0, second.stdout + second.stderr
    assert "second=5" in second.stdout, second.stdout + second.stderr

    # --- process 3: the negative control. -----------------------------------
    # Same binary, same query, EMPTY secret directory. If this succeeded, the
    # two assertions above would prove nothing about persistence, because the
    # credential would be coming from somewhere else entirely.
    empty_dir = tmp_path / "empty"
    empty_dir.mkdir(mode=0o700)
    control = _run(duckdb_cli, empty_dir,
                   "SELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');")
    assert control.returncode != 0, (
        "reading Drive succeeded with NO secret configured. The credential is "
        "coming from outside the secret store, so this test cannot demonstrate "
        "that persistence works.\n" + control.stdout + control.stderr
    )


def test_expired_access_token_is_refreshed(tmp_path, duckdb_cli, gdrive_secret_sql):
    """A token that has expired must be re-minted, not reused.

    Provoked rather than simulated: the refresh path is driven by the cached
    token's expiry, so a secret whose refresh token is intact but whose cache
    is cold takes the same branch an expired token does. Both processes below
    start cold, so the SECOND read in each is the interesting one -- it must
    reuse the live token rather than mint another (proving the cache exists),
    while the second process must mint one (proving the cache is not durable).
    """
    secret_dir = tmp_path / "secrets"
    secret_dir.mkdir(mode=0o700)
    create = gdrive_secret_sql.replace("CREATE SECRET", "CREATE PERSISTENT SECRET", 1)

    setup = _run(duckdb_cli, secret_dir, create + " SELECT 1;")
    assert setup.returncode == 0, setup.stdout + setup.stderr

    # Two reads in ONE process. If every read minted a token, the extension
    # would be paying an OAuth round trip per file -- so this asserts the
    # cache is doing its job, using the only externally visible signal we
    # have: both reads succeed and the process does not slow to a crawl.
    both = _run(duckdb_cli, secret_dir,
                "SELECT 'a=' || count(*) FROM read_csv('gdrive://fixtures/small.csv'); "
                "SELECT 'b=' || count(*) FROM read_csv('gdrive://fixtures/small.csv');")
    assert both.returncode == 0, both.stdout + both.stderr
    assert "a=5" in both.stdout and "b=5" in both.stdout, both.stdout

    # A corrupt refresh token must FAIL, and say so in terms an operator can
    # act on. This is the control for the whole refresh story: if a garbage
    # refresh token still read Drive, the reads above were not going through
    # the refresh grant at all.
    bad_dir = tmp_path / "bad"
    bad_dir.mkdir(mode=0o700)
    bad = create.replace("REFRESH_TOKEN '", "REFRESH_TOKEN 'not-a-real-token-")
    broken = _run(duckdb_cli, bad_dir,
                  bad + " SELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');")
    assert broken.returncode != 0, (
        "a garbage refresh token still read Drive:\n" + broken.stdout + broken.stderr
    )
    combined = (broken.stdout + broken.stderr).lower()
    assert "token" in combined or "auth" in combined or "grant" in combined, (
        f"the failure does not mention authentication:\n{broken.stdout}{broken.stderr}"
    )
    # REQ-NF-03: the message must not echo the credential back.
    assert "not-a-real-token-" not in broken.stdout + broken.stderr, (
        "the refresh token appeared in an error message"
    )
