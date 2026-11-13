"""Turn test/sql/*.test.template into runnable test/sql/live/*.test.

Live SQL tests need ids that do not exist until the fixtures are seeded --
folder ids, file ids, the CI root id. SQLLogicTest has no way to obtain
them, so we substitute them in before running.

Placeholders are ``${NAME}``. Available names:

    ${DRIVE_ID}          the CI root, GDRIVE_CI_DRIVE_ID -- despite the name a
                         plain folder today, bound as ROOT_FOLDER_ID
    ${FIXTURES_ID}       id of /fixtures
    ${SA_KEY_FILE}       path to the service-account key
    ${OAUTH_CLIENT_ID}       delegated-user OAuth client id (GDRIVE_OAUTH_CLIENT_ID)
    ${OAUTH_CLIENT_SECRET}   delegated-user OAuth client secret (GDRIVE_OAUTH_CLIENT_SECRET)
    ${USER_REFRESH_TOKEN}    delegated-user refresh token (GDRIVE_USER_REFRESH_TOKEN)
    ${ID:some/path}      id of that path under /fixtures, resolved live
    ${SHEET:name}        id of a fresh native Google Sheet called `name`, created
                         in this run's scratch folder (tab "Sheet1" holding
                         a,b / 1,2). For the sheet_range tests, which write
                         INTO a Sheet and so cannot share a fixture.

Shared Drive placeholders, for the DRIVE_ID (D-5) tests. They exist only when
GDRIVE_CI_SHARED_DRIVE_ID is set; a template that uses any of them is LEFT OUT
otherwise, with a warning (or fails, under GDRIVE_REQUIRE_SHARED_DRIVE=1):

    ${SHARED_DRIVE_ID}    the Shared Drive id (GDRIVE_CI_SHARED_DRIVE_ID)
    ${SHARED_SCRATCH}     this run's /scratch/run-<id> folder name IN that drive,
                          holding read/small.csv and read/other.csv
    ${SHARED_FIXTURE_ID}  id of read/small.csv there

Note on ${OAUTH_CLIENT_ID} / ${OAUTH_CLIENT_SECRET} / ${USER_REFRESH_TOKEN}: these
carry real credential material, unlike the other placeholders. They are read
straight from the environment at materialisation time and substituted only
into files written under the gitignored test/sql/live/ -- never into anything
tracked by git (scripts/check_no_credentials.sh enforces this).

An unresolved placeholder is a hard error: substituting an empty string would
produce a test that passes against the wrong thing.
"""

from __future__ import annotations

import os
import re
import sys
import uuid
from pathlib import Path

from .drive import Drive, DriveConfigError
from . import fixtures as fx

REPO_ROOT = Path(__file__).resolve().parents[2]
TEMPLATE_DIR = REPO_ROOT / "test" / "sql"
OUT_DIR = REPO_ROOT / "test" / "sql" / "live"

PLACEHOLDER = re.compile(r"\$\{([A-Z_]+)(?::([^}]*))?\}")

#: Placeholders that need GDRIVE_CI_SHARED_DRIVE_ID.
SHARED_PLACEHOLDERS = ("SHARED_DRIVE_ID", "SHARED_SCRATCH", "SHARED_FIXTURE_ID")
#: Where the Shared Drive scratch name is left for run_live_tests.sh's teardown.
SHARED_SCRATCH_FILE = OUT_DIR / ".shared_scratch"
SHARED_SMALL_CSV = b"id,name\n1,a\n2,b\n3,c\n"
SHARED_OTHER_CSV = b"id\n9\n"


def shared_drive_substitutions() -> dict:
    """Per-run scratch in the CI Shared Drive, or {} when none is configured.

    As the delegated user. The scratch folder and its two read fixtures are
    created per run rather than seeded once, so there is no second permanent
    fixture tree to keep in step with seed.py.
    """
    shared_id = os.environ.get("GDRIVE_CI_SHARED_DRIVE_ID", "")
    if not shared_id:
        return {}
    drive = Drive.from_env(prefer_user=True, root_id=shared_id)
    if not drive.is_shared_drive:
        raise RuntimeError(
            f"GDRIVE_CI_SHARED_DRIVE_ID={shared_id} is not a Shared Drive the delegated user is a "
            "member of (drives.get did not answer 200). It must be a Shared Drive id, not a folder."
        )
    scratch_root = drive.find_or_create_folder(fx.SCRATCH_ROOT)
    name = f"run-{uuid.uuid4().hex[:12]}"
    scratch_id = drive.create_folder(name, scratch_root)
    drive.upload(fx.HEARTBEAT_NAME, b"alive\n", scratch_id, "text/plain")
    read_id = drive.create_folder("read", scratch_id)
    small_id = drive.upload("small.csv", SHARED_SMALL_CSV, read_id, "text/csv")
    drive.upload("other.csv", SHARED_OTHER_CSV, read_id, "text/csv")
    SHARED_SCRATCH_FILE.write_text(name + "\n")
    print(f"  shared-drive scratch: /{fx.SCRATCH_ROOT}/{name} ({scratch_id}) in {shared_id}")
    return {"SHARED_DRIVE_ID": shared_id, "SHARED_SCRATCH": name, "SHARED_FIXTURE_ID": small_id}


def needs_shared_drive(text: str) -> bool:
    return any(m.group(1) in SHARED_PLACEHOLDERS for m in PLACEHOLDER.finditer(text))


def build_substitutions(drive: Drive) -> dict:
    found = drive.list_children(drive.drive_id, name=fx.FIXTURES_ROOT)
    if not found:
        raise RuntimeError(
            f"no /{fx.FIXTURES_ROOT} in CI root {drive.drive_id}; run `make seed_fixtures`"
        )
    if len(found) > 1:
        # Drive permits two folders with one name in one parent -- the same
        # R-4 ambiguity the extension refuses to guess through. Taking
        # found[0] would silently bind the whole suite to whichever tree Drive
        # happened to list first, so tests could pass against a stale fixture
        # set. Refuse, and name both.
        ids = ", ".join(f["id"] for f in found)
        raise RuntimeError(
            f"{len(found)} folders named /{fx.FIXTURES_ROOT} exist (ids: {ids}). "
            "The suite cannot tell which is current; delete the stale one."
        )
    key_file = os.environ.get("GDRIVE_CI_SA_KEY_FILE", "")
    if key_file and not Path(key_file).is_absolute():
        key_file = str((REPO_ROOT / key_file).resolve())

    # Delegated-user credentials for the config/refresh-token secret that
    # gdrive_write.test.template creates. Read straight from the environment
    # (never from a file this repo tracks) -- see this module's docstring.
    oauth_client_id = os.environ.get("GDRIVE_OAUTH_CLIENT_ID", "")
    oauth_client_secret = os.environ.get("GDRIVE_OAUTH_CLIENT_SECRET", "")
    user_refresh_token = os.environ.get("GDRIVE_USER_REFRESH_TOKEN", "")

    # Mutating SQL tests need somewhere to write. One scratch folder per
    # materialisation (i.e. per `make test_live` run), carrying a heartbeat so
    # the sweeper can tell a slow run from an abandoned one -- Drive does not
    # advance a folder's own modifiedTime when its children change.
    scratch_root = drive.find_or_create_folder(fx.SCRATCH_ROOT)
    scratch_name = f"run-{uuid.uuid4().hex[:12]}"
    scratch_id = drive.create_folder(scratch_name, scratch_root)
    drive.upload(fx.HEARTBEAT_NAME, b"alive\n", scratch_id, "text/plain")
    print(f"  scratch: /{fx.SCRATCH_ROOT}/{scratch_name} ({scratch_id})")

    # A LOCKED file for gdrive_locks.test: the extension reads locks but
    # cannot set one, so the lock is made here, as the delegated user, in
    # this run's scratch folder. Unlocked again only by the scratch teardown,
    # which deletes the folder (a lock does not stop a delete).
    locked_id = drive.upload(fx.LOCKED_NAME, fx.LOCKED_CONTENT, scratch_id, "text/csv")
    drive.set_lock(locked_id, True, reason=fx.LOCK_REASON, copy_requires_writer_permission=True)

    return {
        "DRIVE_ID": drive.drive_id,
        "FIXTURES_ID": found[0]["id"],
        "SA_KEY_FILE": key_file,
        "OAUTH_CLIENT_ID": oauth_client_id,
        "OAUTH_CLIENT_SECRET": oauth_client_secret,
        "USER_REFRESH_TOKEN": user_refresh_token,
        "SCRATCH": scratch_name,
        "SCRATCH_ID": scratch_id,
        "LOCKED_ID": locked_id,
    }


def render(text: str, subs: dict, drive: Drive, fixtures_id: str, source: str) -> str:
    def replace(m: re.Match) -> str:
        name, arg = m.group(1), m.group(2)
        if name == "ID":
            if not arg:
                raise RuntimeError(f"{source}: ${{ID:...}} needs a path")
            return drive.resolve_path(arg, root_id=fixtures_id)
        if name == "SHEET":
            if not arg:
                raise RuntimeError(f"{source}: ${{SHEET:...}} needs a name")
            sheets = subs.setdefault("_sheets", {})
            if arg not in sheets:
                # A text/csv source (Drive picks the native type from the
                # SOURCE content type -- CLAUDE.md pitfall), tab renamed to
                # Sheet1: a conversion names it after the file.
                sheets[arg] = drive.upload_sheet(arg, b"a,b\n1,2\n", subs["SCRATCH_ID"])
            return sheets[arg]
        if name in subs:
            value = subs[name]
            if not value:
                raise RuntimeError(f"{source}: placeholder ${{{name}}} resolved to empty")
            return value
        raise RuntimeError(f"{source}: unknown placeholder ${{{name}}}")

    return PLACEHOLDER.sub(replace, text)


def main() -> int:
    try:
        # Materialising CREATES a scratch folder and its heartbeat, so it
        # writes -- and a service account cannot write outside a Shared Drive.
        # Read-only tests still run as the service account; only this
        # provisioning step needs the delegated user.
        drive = Drive.from_env(prefer_user=True)
    except DriveConfigError as e:
        print(f"cannot materialise live tests: {e}", file=sys.stderr)
        return 2

    templates = sorted(TEMPLATE_DIR.glob("*.test.template"))
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for stale in OUT_DIR.glob("*.test"):
        stale.unlink()
    SHARED_SCRATCH_FILE.unlink(missing_ok=True)

    if not templates:
        print("no *.test.template found")
        return 0

    subs = build_substitutions(drive)
    shared = shared_drive_substitutions()
    subs.update(shared)
    left_out = []
    for tpl in templates:
        out = OUT_DIR / tpl.name.replace(".test.template", ".test")
        if not shared and needs_shared_drive(tpl.read_text()):
            left_out.append(tpl.name)
            continue
        rendered = render(tpl.read_text(), subs, drive, subs["FIXTURES_ID"], tpl.name)
        # SQLLogicTest reads the `# name:` header; point it at the real file
        # so failures name something that exists on disk.
        rendered = rendered.replace(
            f"# name: test/sql/{tpl.name}", f"# name: test/sql/live/{out.name}"
        )
        out.write_text(rendered)
        print(f"  {tpl.name} -> {out.relative_to(REPO_ROOT)}")

    if left_out:
        # A KNOWN GAP, not a pass: the DRIVE_ID (D-5) path is untested in this
        # run. Loud, so nobody reads the green as covering it; not a failure
        # by default, because no CI Shared Drive exists yet (CLAUDE.md).
        names = ", ".join(left_out)
        if os.environ.get("GDRIVE_REQUIRE_SHARED_DRIVE") == "1":
            print(f"FAIL: GDRIVE_REQUIRE_SHARED_DRIVE=1 but GDRIVE_CI_SHARED_DRIVE_ID is not set; "
                  f"cannot run {names}", file=sys.stderr)
            return 1
        print(f"WARNING: GDRIVE_CI_SHARED_DRIVE_ID is not set, so {names} will NOT run.\n"
              "         The DRIVE_ID (Shared Drive) path is UNVERIFIED in this run -- a known gap,\n"
              "         see CLAUDE.md 'Live-test credentials'.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
