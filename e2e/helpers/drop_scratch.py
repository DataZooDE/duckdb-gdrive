"""Delete one scratch folder by name. Teardown for a live SQL run.

`sweep` is the 24-hour backstop for runs that crashed; this is the ordinary
case. Without it every failed run leaves its tree behind, and the accumulated
noise eventually hides the failure that mattered.
"""

from __future__ import annotations

import os
import sys

from .drive import Drive, DriveConfigError
from . import fixtures as fx


def main() -> int:
    args = sys.argv[1:]
    # --shared-drive: the run's scratch in GDRIVE_CI_SHARED_DRIVE_ID (the
    # DRIVE_ID tests) rather than in the CI root.
    shared = "--shared-drive" in args
    args = [a for a in args if a != "--shared-drive"]
    if len(args) != 1 or not args[0].startswith("run-"):
        print("usage: python -m helpers.drop_scratch [--shared-drive] run-<id>", file=sys.stderr)
        return 2
    name = args[0]
    root_id = None
    if shared:
        root_id = os.environ.get("GDRIVE_CI_SHARED_DRIVE_ID", "")
        if not root_id:
            print("cannot clean up: --shared-drive without GDRIVE_CI_SHARED_DRIVE_ID", file=sys.stderr)
            return 2
    try:
        drive = Drive.from_env(prefer_user=True, root_id=root_id)
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
