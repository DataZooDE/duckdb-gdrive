"""Shared fixtures for the live harness.

Everything here provisions against real Google Drive. If credentials are
absent the whole suite skips with a message that says exactly what to set --
a developer without a Google account must still get a clean run.
"""

from __future__ import annotations

import os
import subprocess
import uuid
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_dotenv() -> None:
    """Load .env.gdrive into os.environ without adding a dependency.

    Values already in the environment win, so CI (which sets real secrets)
    is never overridden by a stale local file.
    """
    env_file = REPO_ROOT / ".env.gdrive"
    if not env_file.exists():
        return
    for line in env_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


_load_dotenv()

from helpers.drive import Drive, credentials_available, user_delegation_available  # noqa: E402
from helpers import fixtures as fx  # noqa: E402


def pytest_configure(config):
    config.addinivalue_line("markers", "live: requires real Google Drive credentials")


@pytest.fixture(scope="session")
def drive() -> Drive:
    if not credentials_available():
        pytest.skip(
            "live Drive credentials not configured -- set GDRIVE_CI_DRIVE_ID and "
            "GDRIVE_CI_SA_KEY_FILE (see .env.gdrive.example)"
        )
    return Drive.from_env()


@pytest.fixture(scope="session")
def writer() -> Drive:
    """A Drive session that can actually CREATE things.

    The `drive` fixture above is the service account, deliberately: that is
    the identity the extension's read path uses, so read assertions should
    exercise it. But a service account has NO Drive storage quota and gets
    403 storageQuotaExceeded on any upload outside a Shared Drive -- and our
    fixture root is an ordinary folder. Folder *creation* still succeeds
    (folders consume no quota), so a scratch fixture built on the service
    account appears to work right up until the first byte is written.

    That is exactly how this surfaced: the SQL suite was green and the pytest
    harness failed in CI on `upload failed: HTTP 403`.

    Writes therefore run as the delegated user, matching seeding and
    materialisation (helpers/seed.py, helpers/materialise.py).
    """
    if not user_delegation_available():
        pytest.skip(
            "writing needs a delegated user token: a service account has no Drive "
            "storage quota. Run `make oauth_consent`."
        )
    return Drive.from_env(prefer_user=True)


@pytest.fixture(scope="session")
def fixtures_root(drive: Drive) -> str:
    """Id of the permanent, read-only /fixtures folder.

    Fails rather than seeds: seeding is an explicit `make seed_fixtures` step,
    because silently uploading 100MB during a test run is a surprise nobody
    wants on a metered connection.
    """
    found = drive.list_children(drive.drive_id, name=fx.FIXTURES_ROOT)
    if not found:
        pytest.fail(
            f"no /{fx.FIXTURES_ROOT} folder in Shared Drive {drive.drive_id}. "
            "Run `make seed_fixtures` first."
        )
    if len(found) > 1:
        # See materialise.py: picking one would bind every test to whichever
        # tree Drive listed first, which is how a suite ends up passing
        # against stale fixtures.
        ids = ", ".join(f["id"] for f in found)
        pytest.fail(f"{len(found)} folders named /{fx.FIXTURES_ROOT} exist (ids: {ids}); "
                    "delete the stale one.")
    return found[0]["id"]


@pytest.fixture
def scratch(writer: Drive):
    """A per-test scratch folder, deleted on teardown.

    Per-test rather than per-session so a failing test cannot leave state that
    breaks the next one, and so concurrent CI runs never collide.

    Built on `writer`, not `drive`: this creates files, and the service
    account has no storage quota to create them with. See the writer fixture.
    """
    drive = writer
    scratch_root = drive.find_or_create_folder(fx.SCRATCH_ROOT)
    name = f"run-{uuid.uuid4().hex[:12]}"
    folder_id = drive.create_folder(name, scratch_root)

    # Heartbeat. The sweeper cannot rely on the folder's own modifiedTime:
    # Drive does not advance a folder's timestamp when its children change, so
    # a long-running test would look idle and could be swept out from under
    # itself. An explicit heartbeat file, refreshed on creation and touched on
    # teardown, gives the sweeper something that actually tracks liveness.
    drive.upload(fx.HEARTBEAT_NAME, b"alive\n", folder_id, "text/plain")

    try:
        yield folder_id
    finally:
        try:
            drive.delete(folder_id, permanent=True)
        except Exception as e:  # teardown must not mask a test failure
            print(f"WARNING: could not clean up scratch {name}: {e}")


@pytest.fixture(scope="session")
def gdrive_secret_sql() -> str:
    """CREATE SECRET for a delegated user, as SQL.

    The delegated user rather than the service account because tests using
    this may write, and a service account has no Drive storage quota outside
    a Shared Drive. Values come from the environment and are never written to
    anything git tracks.
    """
    needed = ("GDRIVE_OAUTH_CLIENT_ID", "GDRIVE_OAUTH_CLIENT_SECRET",
              "GDRIVE_USER_REFRESH_TOKEN", "GDRIVE_CI_DRIVE_ID")
    missing = [n for n in needed if not os.environ.get(n)]
    if missing:
        pytest.skip(f"missing {', '.join(missing)}; run `make oauth_consent`")
    return (
        "CREATE SECRET gdrive_e2e (TYPE gdrive, PROVIDER config, "
        f"CLIENT_ID '{os.environ['GDRIVE_OAUTH_CLIENT_ID']}', "
        f"CLIENT_SECRET '{os.environ['GDRIVE_OAUTH_CLIENT_SECRET']}', "
        f"REFRESH_TOKEN '{os.environ['GDRIVE_USER_REFRESH_TOKEN']}', "
        f"ROOT_FOLDER_ID '{os.environ['GDRIVE_CI_DRIVE_ID']}', "
        "DRIVE_SCOPE 'https://www.googleapis.com/auth/drive');"
    )


@pytest.fixture(scope="session")
def duckdb_cli() -> Path:
    """The built duckdb shell with the extension statically linked."""
    exe = REPO_ROOT / "build" / "release" / "duckdb"
    if not exe.exists():
        pytest.skip(f"{exe} not built -- run `make` first")
    return exe


@pytest.fixture(scope="session")
def sql(duckdb_cli: Path):
    """Run SQL through the built duckdb shell, returning stdout.

    Uses the real CLI rather than the Python duckdb package so the extension
    under test is exactly the artifact we ship, not a separately built one.
    """
    def _run(statements: str, expect_error: bool = False) -> str:
        proc = subprocess.run(
            [str(duckdb_cli), "-noheader", "-list", "-c", statements],
            capture_output=True, text=True, timeout=600,
        )
        output = proc.stdout + proc.stderr
        if expect_error:
            if proc.returncode == 0:
                raise AssertionError(f"expected an error, got success:\n{output}")
        elif proc.returncode != 0:
            raise AssertionError(f"duckdb failed ({proc.returncode}):\n{output}")
        return output.strip()

    return _run
