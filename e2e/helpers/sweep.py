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


def _parse_rfc3339(value: str) -> dt.datetime:
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))


def _liveness_time(drive: Drive, folder: dict) -> dt.datetime:
    """Most recent sign of life for a scratch folder.

    Prefers the heartbeat file's modifiedTime; falls back to the folder's own
    when there is no heartbeat (a folder created before heartbeats existed, or
    by a run that died between mkdir and the first write).
    """
    try:
        beats = drive.list_children(folder["id"], name=fx.HEARTBEAT_NAME)
    except Exception:
        beats = []
    if beats:
        return _parse_rfc3339(beats[0]["modifiedTime"])
    return _parse_rfc3339(folder["modifiedTime"])


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

        # Liveness comes from the heartbeat file, not the folder. Drive does
        # NOT advance a folder's modifiedTime when its children change, so a
        # folder belonging to a slow-but-active run looks exactly as stale as
        # one abandoned yesterday. Deleting a live run's scratch mid-test
        # produces a baffling failure in an unrelated job, so prefer the
        # heartbeat and fall back to the folder only when it is absent.
        modified = _liveness_time(drive, child)
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
