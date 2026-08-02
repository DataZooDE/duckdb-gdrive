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


class SweepIncomplete(RuntimeError):
    """Some folders could not be deleted. The sweep ran; it did not finish."""


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
    unswept = []
    failed: list[tuple[str, str]] = []
    for child in drive.list_children(scratch):
        if child["mimeType"] != FOLDER_MIME or not child["name"].startswith("run-"):
            # The run- prefix is deliberate: it protects anything a HUMAN put
            # under /scratch from being deleted by a cron job. But skipping
            # silently means such a folder is never collected and nobody ever
            # learns it is there. An ad-hoc `lake-<ts>` folder from a manual
            # DuckLake experiment sat here doing exactly that. Report them.
            if child["mimeType"] == FOLDER_MIME:
                unswept.append(child["name"])
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
            # One folder we cannot delete must not strand the other sixty.
            #
            # Both identities write here -- the service account for reads, the
            # delegated user for writes -- and permanent deletion in a Shared
            # Drive needs a role the deleting identity may not hold for a
            # folder the other one created. Before this, the first
            # `insufficientFilePermissions` aborted the whole sweep: a real run
            # removed exactly ONE folder of 61 and exited, and because the
            # nightly job was separately broken nobody saw it for a week.
            #
            # Failures are collected and reported, and the exit status is
            # non-zero, so this is visible rather than silently partial.
            try:
                drive.delete(child["id"], permanent=True)
            except Exception as e:  # noqa: BLE001 -- any failure, keep going
                failed.append((child["name"], str(e).splitlines()[0][:120]))
                continue
        removed += 1

    print(f"{removed} stale scratch folder(s) {'found' if dry_run else 'removed'}")
    if failed:
        print(f"FAILED to delete {len(failed)} folder(s):")
        for name, err in failed:
            print(f"  {name}: {err}")
        print("  Usually a Shared Drive role: permanent deletion needs Manager")
        print("  on a folder the other CI identity created.")
    if unswept:
        print(f"NOTE: {len(unswept)} folder(s) under /{fx.SCRATCH_ROOT} do not use the "
              f"run-<uuid> convention and will NEVER be swept:")
        for name in sorted(unswept):
            print(f"  {name}")
        print("  Delete them by hand, or rename them to run-<something>.")
    if failed:
        raise SweepIncomplete(f"{len(failed)} folder(s) could not be deleted")
    return removed


def main() -> int:
    try:
        # As the DELEGATED USER, not the service account.
        #
        # Scratch folders are created by writes, and writes run as the user
        # (a service account has no Drive storage quota). The user therefore
        # OWNS them, and a service account cannot permanently delete a file it
        # does not own in a Shared Drive. Running this as the SA fails with
        # 403 insufficientFilePermissions on every folder -- measured
        # 2026-08-02: 0 of 58 deletable as the SA, 58 of 58 as the user.
        drive = Drive.from_env(prefer_user=True)
    except DriveConfigError as e:
        print(f"cannot sweep: {e}", file=sys.stderr)
        return 2
    max_age = int(os.environ.get("GDRIVE_SWEEP_MAX_AGE_HOURS", DEFAULT_MAX_AGE_HOURS))
    dry = os.environ.get("GDRIVE_SWEEP_DRY_RUN") == "1"
    try:
        sweep(drive, max_age_hours=max_age, dry_run=dry)
    except SweepIncomplete as e:
        # Non-zero: a sweep that silently leaves folders behind is how the
        # Shared Drive fills up while the job reports success every night.
        print(f"sweep incomplete: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
