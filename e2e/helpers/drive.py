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


def _user_credentials():
    """Delegated-user credentials from the stored refresh token, or None.

    Deliberately hand-rolled rather than google-auth's UserCredentials: the
    harness must stay a thin, inspectable mirror of what the extension itself
    does, so a divergence between them is visible rather than hidden behind
    an SDK.
    """
    refresh = os.environ.get("GDRIVE_USER_REFRESH_TOKEN")
    client_id = os.environ.get("GDRIVE_OAUTH_CLIENT_ID")
    client_secret = os.environ.get("GDRIVE_OAUTH_CLIENT_SECRET")
    if not (refresh and client_id and client_secret):
        return None

    resp = requests.post("https://oauth2.googleapis.com/token", data={
        "client_id": client_id,
        "client_secret": client_secret,
        "refresh_token": refresh,
        "grant_type": "refresh_token",
    }, timeout=60)
    if resp.status_code != 200:
        # Never echo the body: a token endpoint's error can quote the request.
        raise DriveConfigError(
            f"refreshing the delegated user token failed (HTTP {resp.status_code}). "
            "Re-run scripts/oauth_consent.py."
        )
    return _StaticToken(resp.json()["access_token"])


class _StaticToken:
    """Minimal stand-in for a google-auth credential: a token and nothing else."""

    def __init__(self, token: str):
        self.token = token
        self.valid = True

    def refresh(self, _request) -> None:
        return None


def user_delegation_available() -> bool:
    return bool(
        os.environ.get("GDRIVE_USER_REFRESH_TOKEN")
        and os.environ.get("GDRIVE_OAUTH_CLIENT_ID")
        and os.environ.get("GDRIVE_OAUTH_CLIENT_SECRET")
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
    #: "service_account" or "user" -- which identity this session acts as.
    #: Surfaced because a 403 means very different things for each.
    _identity: str = "service_account"
    #: True when drive_id names an actual Shared Drive rather than an ordinary
    #: folder. Detected once, because the two need different query parameters
    #: and guessing wrong produces a misleading 404.
    is_shared_drive: bool = False
    #: Count of HTTP calls by kind. The harness asserts on this for the R-1
    #: metadata-amplification checks -- it is the ground truth against which
    #: the extension's own gdrive_stats() counter is validated.
    calls: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls, prefer_user: bool = False) -> "Drive":
        """Authenticate, as the user when possible and needed.

        Two identities, for a reason that is not a preference:

        * The **service account** can read anything shared with it, but has no
          Drive storage quota, so it cannot create a single fixture file
          outside a Shared Drive (403 storageQuotaExceeded). Reads: fine.
          Seeding: impossible.
        * The **delegated user** (a stored refresh token from
          scripts/oauth_consent.py) writes against their own quota.

        So seeding runs as the user and the read tests run as the service
        account, against the same folder. `prefer_user` selects which.
        """
        drive_id = os.environ.get("GDRIVE_CI_DRIVE_ID")
        if not drive_id:
            raise DriveConfigError(
                "GDRIVE_CI_DRIVE_ID is not set (a Shared Drive id, or a folder "
                "id shared with the service account). See "
                "scripts/setup_ci_drive.sh."
            )

        if prefer_user:
            user_creds = _user_credentials()
            if user_creds is None:
                raise DriveConfigError(
                    "writing needs a delegated user token: a service account has no "
                    "Drive storage quota and cannot create files outside a Shared "
                    "Drive. Run:  uv run --with requests python scripts/oauth_consent.py"
                )
            d = cls(drive_id=drive_id, _creds=user_creds, _identity="user")
            d.is_shared_drive = d._detect_shared_drive()
            return d

        creds = service_account.Credentials.from_service_account_file(
            str(_key_path()), scopes=SCOPES
        )
        creds.refresh(gauth_requests.Request())
        d = cls(drive_id=drive_id, _creds=creds, _identity="service_account")
        d.is_shared_drive = d._detect_shared_drive()
        return d

    # -- plumbing ----------------------------------------------------------

    def _headers(self) -> dict:
        if not self._creds.valid:
            self._creds.refresh(gauth_requests.Request())
        return {"Authorization": f"Bearer {self._creds.token}"}

    def _request(self, method: str, url: str, kind: str, **kw) -> requests.Response:
        self.calls[kind] = self.calls.get(kind, 0) + 1
        headers = {**self._headers(), **kw.pop("headers", {})}
        params = {**SHARED_DRIVE_PARAMS, **kw.pop("params", {})}
        # A flat 60s covers every metadata call with room to spare, but it is
        # far too short for the 100 MB benchmark fixture: the socket write
        # timed out mid-body and seeding died with a bare ConnectionError that
        # named neither the file nor the size. Callers moving bulk bytes pass
        # their own budget; see upload().
        timeout = kw.pop("timeout", 60)
        resp = requests.request(method, url, headers=headers, params=params,
                                timeout=timeout, **kw)
        return resp

    @staticmethod
    def _transfer_timeout(nbytes: int) -> int:
        """Seconds to allow for moving `nbytes`, floor 60.

        Assumes a pessimistic 1 MB/s -- slow enough to cover a bad hotel link
        and a CI runner under load, while still bounding a genuinely wedged
        connection rather than hanging forever.
        """
        return max(60, 60 + int(nbytes / 1e6))

    def _ok(self, resp: requests.Response, what: str) -> dict:
        if resp.status_code not in (200, 201, 204):
            raise RuntimeError(f"{what} failed: HTTP {resp.status_code}: {resp.text[:500]}")
        return resp.json() if resp.content else {}

    def _detect_shared_drive(self) -> bool:
        """Is drive_id a Shared Drive, or an ordinary folder?

        A Shared Drive answers drives.get; a folder 404s. Cheaper and more
        honest than pattern-matching the id prefix.
        """
        resp = self._request("GET", f"{DRIVE_V3}/drives/{self.drive_id}", "drives.get")
        return resp.status_code == 200

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
                "fields": "nextPageToken, files(id, name, mimeType, size, modifiedTime, md5Checksum)",
                "pageSize": "1000",
            }
            # corpora=drive + driveId are ONLY valid for a real Shared Drive.
            # Passing them for an ordinary folder id yields
            # 404 "Shared drive not found", which reads like a permissions
            # problem and is nothing of the sort. The root here may legitimately
            # be either, so scope only when it really is a Shared Drive.
            if self.is_shared_drive:
                params["corpora"] = "drive"
                params["driveId"] = self.drive_id
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
        # A whole-file download has no declared size until the response
        # arrives, and the benchmark fixture is 100 MB -- budget for the
        # unranged case as if it were large, and for a ranged one by its span.
        nbytes = (byte_range[1] - byte_range[0] + 1) if byte_range else 512_000_000
        if byte_range is not None:
            headers["Range"] = f"bytes={byte_range[0]}-{byte_range[1]}"
        resp = self._request(
            "GET", f"{DRIVE_V3}/files/{file_id}", "files.get.media",
            params={"alt": "media"}, headers=headers,
            timeout=self._transfer_timeout(nbytes),
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
            timeout=self._transfer_timeout(len(content)),
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
