"""Delete one scratch folder by name. Teardown for a live SQL run.

`sweep` is the 24-hour backstop for runs that crashed; this is the ordinary
case. Without it every failed run leaves its tree behind, and the accumulated
noise eventually hides the failure that mattered.
"""

from __future__ import annotations

import sys

from .drive import Drive, DriveConfigError
from . import fixtures as fx


def main() -> int:
    if len(sys.argv) != 2 or not sys.argv[1].startswith("run-"):
        print("usage: python -m helpers.drop_scratch run-<id>", file=sys.stderr)
        return 2
    name = sys.argv[1]
    try:
        drive = Drive.from_env(prefer_user=True)
    except DriveConfigError as e:
        print(f"cannot clean up: {e}", file=sys.stderr)
        return 2
    root = drive.find_or_create_folder(fx.SCRATCH_ROOT)
    for child in drive.list_children(root, name=name):
        drive.delete(child["id"], permanent=True)
        print(f"removed /{fx.SCRATCH_ROOT}/{name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
