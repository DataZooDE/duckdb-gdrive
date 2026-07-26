"""Turn test/sql/*.test.template into runnable test/sql/live/*.test.

Live SQL tests need ids that do not exist until the fixtures are seeded --
folder ids, file ids, the Shared Drive id. SQLLogicTest has no way to obtain
them, so we substitute them in before running.

Placeholders are ``${NAME}``. Available names:

    ${DRIVE_ID}          the Shared Drive id
    ${FIXTURES_ID}       id of /fixtures
    ${SA_KEY_FILE}       path to the service-account key
    ${ID:some/path}      id of that path under /fixtures, resolved live

An unresolved placeholder is a hard error: substituting an empty string would
produce a test that passes against the wrong thing.
"""

from __future__ import annotations

import os
import re
import sys
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
    return {
        "DRIVE_ID": drive.drive_id,
        "FIXTURES_ID": found[0]["id"],
        "SA_KEY_FILE": key_file,
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
        drive = Drive.from_env()
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
