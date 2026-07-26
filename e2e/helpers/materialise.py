"""Turn test/sql/*.test.template into runnable test/sql/live/*.test.

Live SQL tests need ids that do not exist until the fixtures are seeded --
folder ids, file ids, the Shared Drive id. SQLLogicTest has no way to obtain
them, so we substitute them in before running.

Placeholders are ``${NAME}``. Available names:

    ${DRIVE_ID}          the Shared Drive id
    ${FIXTURES_ID}       id of /fixtures
    ${SA_KEY_FILE}       path to the service-account key
    ${OAUTH_CLIENT_ID}       delegated-user OAuth client id (GDRIVE_OAUTH_CLIENT_ID)
    ${OAUTH_CLIENT_SECRET}   delegated-user OAuth client secret (GDRIVE_OAUTH_CLIENT_SECRET)
    ${USER_REFRESH_TOKEN}    delegated-user refresh token (GDRIVE_USER_REFRESH_TOKEN)
    ${ID:some/path}      id of that path under /fixtures, resolved live

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


def build_substitutions(drive: Drive) -> dict:
    found = drive.list_children(drive.drive_id, name=fx.FIXTURES_ROOT)
    if not found:
        raise RuntimeError(
            f"no /{fx.FIXTURES_ROOT} in Shared Drive {drive.drive_id}; run `make seed_fixtures`"
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

    return {
        "DRIVE_ID": drive.drive_id,
        "FIXTURES_ID": found[0]["id"],
        "SA_KEY_FILE": key_file,
        "OAUTH_CLIENT_ID": oauth_client_id,
        "OAUTH_CLIENT_SECRET": oauth_client_secret,
        "USER_REFRESH_TOKEN": user_refresh_token,
        "SCRATCH": scratch_name,
        "SCRATCH_ID": scratch_id,
    }


def render(text: str, subs: dict, drive: Drive, fixtures_id: str, source: str) -> str:
    def replace(m: re.Match) -> str:
        name, arg = m.group(1), m.group(2)
        if name == "ID":
            if not arg:
                raise RuntimeError(f"{source}: ${{ID:...}} needs a path")
            return drive.resolve_path(arg, root_id=fixtures_id)
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

    if not templates:
        print("no *.test.template found")
        return 0

    subs = build_substitutions(drive)
    for tpl in templates:
        out = OUT_DIR / tpl.name.replace(".test.template", ".test")
        rendered = render(tpl.read_text(), subs, drive, subs["FIXTURES_ID"], tpl.name)
        # SQLLogicTest reads the `# name:` header; point it at the real file
        # so failures name something that exists on disk.
        rendered = rendered.replace(
            f"# name: test/sql/{tpl.name}", f"# name: test/sql/live/{out.name}"
        )
        out.write_text(rendered)
        print(f"  {tpl.name} -> {out.relative_to(REPO_ROOT)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
