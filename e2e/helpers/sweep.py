"""Delete stale per-run scratch folders.

Crashed runs leak their ``/scratch/run-<uuid>`` folder: teardown never ran.
Left alone these accumulate until the Shared Drive fills up and every test
starts failing for a reason that has nothing to do with the code.

Run by ``make sweep_orphans`` and by a nightly CI job.

The age cutoff is deliberately generous (24h by default) so a long-running
local session is never swept out from under itself.
"""

from __future__ import annotations

import datetime as dt
import os
import sys

from .drive import Drive, DriveConfigError, FOLDER_MIME
from . import fixtures as fx

DEFAULT_MAX_AGE_HOURS = 24


def sweep(drive: Drive, max_age_hours: int = DEFAULT_MAX_AGE_HOURS, dry_run: bool = False) -> int:
    roots = drive.list_children(drive.drive_id, name=fx.SCRATCH_ROOT)
    if not roots:
        print("no /scratch folder; nothing to sweep")
        return 0
    scratch = roots[0]["id"]

    cutoff = dt.datetime.now(dt.timezone.utc) - dt.timedelta(hours=max_age_hours)
    removed = 0
    for child in drive.list_children(scratch):
        if child["mimeType"] != FOLDER_MIME or not child["name"].startswith("run-"):
            continue
        modified = dt.datetime.fromisoformat(child["modifiedTime"].replace("Z", "+00:00"))
        if modified >= cutoff:
            continue
        age = dt.datetime.now(dt.timezone.utc) - modified
        print(f"{'would delete' if dry_run else 'deleting'} {child['name']} "
              f"(age {age.total_seconds() / 3600:.1f}h)")
        if not dry_run:
            drive.delete(child["id"], permanent=True)
        removed += 1

    print(f"{removed} stale scratch folder(s) {'found' if dry_run else 'removed'}")
    return removed


def main() -> int:
    try:
        drive = Drive.from_env()
    except DriveConfigError as e:
        print(f"cannot sweep: {e}", file=sys.stderr)
        return 2
    max_age = int(os.environ.get("GDRIVE_SWEEP_MAX_AGE_HOURS", DEFAULT_MAX_AGE_HOURS))
    dry = os.environ.get("GDRIVE_SWEEP_DRY_RUN") == "1"
    sweep(drive, max_age_hours=max_age, dry_run=dry)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
