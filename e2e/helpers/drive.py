"""Thin, dependency-light Google Drive v3 client for the test harness.

Deliberately raw `requests` rather than google-api-python-client: the harness
must be able to assert on exact HTTP behaviour (status codes, Range handling,
pagination) and a high-level SDK hides precisely the things we are testing.

This is harness code, not a fake. Every call here hits real Google Drive.
"""

from __future__ import annotations

import atexit
import base64
import json
import os
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

import google.auth.transport.requests as gauth_requests
import requests
from google.oauth2 import service_account

DRIVE_V3 = "https://www.googleapis.com/drive/v3"
UPLOAD_V3 = "https://www.googleapis.com/upload/drive/v3"
FOLDER_MIME = "application/vnd.google-apps.folder"
SCOPES = ["https://www.googleapis.com/auth/drive"]

# Every request carries these. Without them the API silently pretends Shared
# Drives do not exist, which produces "not found" for files that plainly do --
# an afternoon lost the first time you hit it.
SHARED_DRIVE_PARAMS = {
    "supportsAllDrives": "true",
    "includeItemsFromAllDrives": "true",
}


class DriveConfigError(RuntimeError):
    """Credentials or Shared Drive not configured."""


#: The one temp key this process materialised from base64, if any. Module-level
#: so repeated _key_path() calls reuse it instead of scattering copies of a
#: private key across /tmp -- the original bug here was that
#: credentials_available() and Drive.from_env() each wrote their own, and
#: neither deleted it.
_materialised_key: "Path | None" = None


def _cleanup_materialised_key() -> None:
    global _materialised_key
    if _materialised_key is not None:
        try:
            _materialised_key.unlink(missing_ok=True)
        except OSError:
            pass
        _materialised_key = None


atexit.register(_cleanup_materialised_key)


def _key_path() -> Path:
    """Resolve the service-account key, preferring the file, then base64.

    The base64 form exists for CI, where the key arrives as a GitHub secret.
    It is written to a 0600 temp file because google-auth wants a path.

    That temp file is created **once per process**, reused thereafter, and
    removed by an atexit hook. Cleanup on a hard kill (SIGKILL, OOM) is not
    possible from here and is therefore best-effort -- CI runners are
    ephemeral, but a developer running this locally should know a decoded key
    can survive a crash in their temp directory.
    """
    global _materialised_key

    explicit = os.environ.get("GDRIVE_CI_SA_KEY_FILE")
    if explicit:
        p = Path(explicit).expanduser()
        if not p.is_absolute():
            p = (Path(__file__).resolve().parents[2] / explicit).resolve()
        if p.exists():
            return p
        raise DriveConfigError(f"GDRIVE_CI_SA_KEY_FILE points at a missing file: {p}")

    if _materialised_key is not None and _materialised_key.exists():
        return _materialised_key

    b64 = os.environ.get("GDRIVE_CI_SA_KEY_B64")
    if b64:
        fd, name = tempfile.mkstemp(suffix=".json", prefix="gdrive-ci-key-")
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as fh:
            fh.write(base64.b64decode(b64))
        _materialised_key = Path(name)
        # Children (the duckdb CLI running live SQL tests) need the same path,
        # and the SQL templates substitute ${SA_KEY_FILE} from this variable.
        os.environ.setdefault("GDRIVE_CI_SA_KEY_FILE", name)
        return _materialised_key

    raise DriveConfigError(
        "No service-account key. Set GDRIVE_CI_SA_KEY_FILE or GDRIVE_CI_SA_KEY_B64 "
        "(see .env.gdrive.example)."
    )


def credentials_available() -> bool:
    """True when the live suite can run. Used to skip rather than fail."""
    if not os.environ.get("GDRIVE_CI_DRIVE_ID"):
        return False
    try:
        _key_path()
    except DriveConfigError:
        return False
    return True


@dataclass
class Drive:
    """Authenticated Drive v3 session scoped to one Shared Drive."""

    drive_id: str
    _creds: object = field(default=None, repr=False)
    #: Count of HTTP calls by kind. The harness asserts on this for the R-1
    #: metadata-amplification checks -- it is the ground truth against which
    #: the extension's own gdrive_stats() counter is validated.
    calls: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls) -> "Drive":
        drive_id = os.environ.get("GDRIVE_CI_DRIVE_ID")
        if not drive_id:
            raise DriveConfigError(
                "GDRIVE_CI_DRIVE_ID is not set. A Shared Drive is required: a "
                "service account has no personal Drive storage quota. See "
                "scripts/setup_ci_drive.sh."
            )
        creds = service_account.Credentials.from_service_account_file(
            str(_key_path()), scopes=SCOPES
        )
        creds.refresh(gauth_requests.Request())
        return cls(drive_id=drive_id, _creds=creds)

    # -- plumbing ----------------------------------------------------------

    def _headers(self) -> dict:
        if not self._creds.valid:
            self._creds.refresh(gauth_requests.Request())
        return {"Authorization": f"Bearer {self._creds.token}"}

    def _request(self, method: str, url: str, kind: str, **kw) -> requests.Response:
        self.calls[kind] = self.calls.get(kind, 0) + 1
        headers = {**self._headers(), **kw.pop("headers", {})}
        params = {**SHARED_DRIVE_PARAMS, **kw.pop("params", {})}
        resp = requests.request(method, url, headers=headers, params=params, timeout=60, **kw)
        return resp

    def _ok(self, resp: requests.Response, what: str) -> dict:
        if resp.status_code not in (200, 201, 204):
            raise RuntimeError(f"{what} failed: HTTP {resp.status_code}: {resp.text[:500]}")
        return resp.json() if resp.content else {}

    def reset_call_counts(self) -> None:
        self.calls.clear()

    # -- reads -------------------------------------------------------------

    def list_children(self, parent_id: str, name: str | None = None) -> list[dict]:
        """All children of a folder, following pagination to the end.

        Pagination is not optional: Drive caps pageSize at 1000 and defaults to
        100. A harness that reads only the first page will happily report that
        a 150-file glob fixture contains 100 files.
        """
        q = f"'{parent_id}' in parents and trashed = false"
        if name is not None:
            escaped = name.replace("\\", "\\\\").replace("'", "\\'")
            q += f" and name = '{escaped}'"
        out: list[dict] = []
        page_token = None
        while True:
            params = {
                "q": q,
                "corpora": "drive",
                "driveId": self.drive_id,
                "fields": "nextPageToken, files(id, name, mimeType, size, modifiedTime, md5Checksum)",
                "pageSize": "1000",
            }
            if page_token:
                params["pageToken"] = page_token
            data = self._ok(
                self._request("GET", f"{DRIVE_V3}/files", "files.list", params=params),
                "files.list",
            )
            out.extend(data.get("files", []))
            page_token = data.get("nextPageToken")
            if not page_token:
                return out

    def get_metadata(self, file_id: str) -> dict:
        return self._ok(
            self._request(
                "GET",
                f"{DRIVE_V3}/files/{file_id}",
                "files.get",
                params={"fields": "id, name, mimeType, size, modifiedTime, headRevisionId, md5Checksum"},
            ),
            "files.get",
        )

    def download(self, file_id: str, byte_range: tuple[int, int] | None = None) -> bytes:
        headers = {}
        if byte_range is not None:
            headers["Range"] = f"bytes={byte_range[0]}-{byte_range[1]}"
        resp = self._request(
            "GET", f"{DRIVE_V3}/files/{file_id}", "files.get.media",
            params={"alt": "media"}, headers=headers,
        )
        if resp.status_code not in (200, 206):
            raise RuntimeError(f"download failed: HTTP {resp.status_code}: {resp.text[:300]}")
        return resp.content

    def export(self, file_id: str, mime: str) -> bytes:
        resp = self._request(
            "GET", f"{DRIVE_V3}/files/{file_id}/export", "files.export",
            params={"mimeType": mime},
        )
        if resp.status_code != 200:
            raise RuntimeError(f"export failed: HTTP {resp.status_code}: {resp.text[:300]}")
        return resp.content

    def resolve_path(self, path: str, root_id: str | None = None) -> str:
        """Walk a slash-separated path to a file id, one files.list per segment.

        This mirrors what the extension's resolver must do, and exists so tests
        can compute the expected id independently of the code under test.
        """
        current = root_id or self.drive_id
        for segment in [s for s in path.split("/") if s]:
            matches = self.list_children(current, name=segment)
            if not matches:
                raise FileNotFoundError(f"no such path segment {segment!r} in {path!r}")
            if len(matches) > 1:
                ids = ", ".join(m["id"] for m in matches)
                raise RuntimeError(f"ambiguous segment {segment!r} in {path!r}: {ids}")
            current = matches[0]["id"]
        return current

    # -- writes ------------------------------------------------------------

    def create_folder(self, name: str, parent_id: str | None = None) -> str:
        body = {
            "name": name,
            "mimeType": FOLDER_MIME,
            "parents": [parent_id or self.drive_id],
        }
        return self._ok(
            self._request("POST", f"{DRIVE_V3}/files", "files.create", json=body),
            "create_folder",
        )["id"]

    def upload(self, name: str, content: bytes, parent_id: str, mime: str = "application/octet-stream") -> str:
        meta = {"name": name, "parents": [parent_id]}
        files = {
            "metadata": ("metadata.json", json.dumps(meta), "application/json"),
            "file": (name, content, mime),
        }
        resp = self._request(
            "POST", f"{UPLOAD_V3}/files", "files.create.upload",
            params={"uploadType": "multipart"}, files=files,
        )
        return self._ok(resp, "upload")["id"]

    def upload_native(self, name: str, csv_content: bytes, parent_id: str, target_mime: str) -> str:
        """Upload and convert to a native Google format (Sheet / Doc).

        Needed for the REQ-F-07 fixtures: a native file has no bytes, so
        `alt=media` fails on it and `files.export` is the only way to read it.
        """
        meta = {"name": name, "parents": [parent_id], "mimeType": target_mime}
        files = {
            "metadata": ("metadata.json", json.dumps(meta), "application/json"),
            "file": (name, csv_content, "text/csv"),
        }
        resp = self._request(
            "POST", f"{UPLOAD_V3}/files", "files.create.upload",
            params={"uploadType": "multipart"}, files=files,
        )
        return self._ok(resp, "upload_native")["id"]

    def delete(self, file_id: str, permanent: bool = True) -> None:
        if permanent:
            resp = self._request("DELETE", f"{DRIVE_V3}/files/{file_id}", "files.delete")
            if resp.status_code not in (200, 204, 404):
                raise RuntimeError(f"delete failed: HTTP {resp.status_code}: {resp.text[:300]}")
        else:
            self._ok(
                self._request("PATCH", f"{DRIVE_V3}/files/{file_id}", "files.update",
                              json={"trashed": True}),
                "trash",
            )

    def find_or_create_folder(self, name: str, parent_id: str | None = None) -> str:
        """mkdir -p for one level, safe against a concurrent run doing the same.

        Drive has no atomic create-if-absent, so two jobs can both see no
        `/scratch`, both create one, and then silently diverge -- one run's
        fixtures land in a folder the other never looks at. Returning
        `existing[0]` would hide that indefinitely.

        We cannot prevent the race, so we detect it: after creating, re-list
        and require exactly one. Two folders with one name is harness
        corruption and must be loud, because every later "file not found" is
        then a lie.
        """
        parent = parent_id or self.drive_id

        def folders() -> list[dict]:
            return [f for f in self.list_children(parent, name=name)
                    if f["mimeType"] == FOLDER_MIME]

        existing = folders()
        if len(existing) == 1:
            return existing[0]["id"]
        if len(existing) > 1:
            ids = ", ".join(f["id"] for f in existing)
            raise RuntimeError(
                f"{len(existing)} folders named {name!r} under {parent}: {ids}. "
                "This is harness corruption, usually from two runs racing to create "
                "it. Delete all but one by hand before continuing."
            )

        created = self.create_folder(name, parent)

        # Re-check: another run may have created its own between our list and
        # our create.
        after = folders()
        if len(after) > 1:
            ids = ", ".join(f["id"] for f in after)
            raise RuntimeError(
                f"race creating folder {name!r} under {parent}: now {len(after)} "
                f"exist ({ids}). Delete all but one by hand. Seeding and live "
                "tests should not run concurrently."
            )
        return created
