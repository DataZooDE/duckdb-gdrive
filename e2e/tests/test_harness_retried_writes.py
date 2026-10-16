"""The harness's own creates survive a lost response without duplicating.

CI flaked twice on 2026-10-15 with the same shape: a create POST from the
harness (the scratch heartbeat upload) waited 60 s for www.googleapis.com and
raised ReadTimeout. A POST was deliberately never retried, because a dropped
response is ambiguous -- the file may exist -- and a blind replay makes a
second same-named file. The harness now uses the extension's own remedy:
reserve the id with files.generateIds, so a replay lands on 409 instead of
creating a duplicate (see test_idempotent_create.py for Drive's side of that).

The fault is injected around the REAL request, against the real Drive: the
wrapper either sends the request and then loses the response (the ambiguous
case) or fails before sending (the plain case). Nothing about Drive's
behaviour is simulated.
"""

from __future__ import annotations

import json

import pytest
import requests

import helpers.drive as drive_mod

pytestmark = pytest.mark.live


def _lose_first(monkeypatch, method: str, *, after_sending: bool) -> list[str]:
    """Make the first `method` request raise ReadTimeout, sent or not."""
    real = requests.request
    seen: list[str] = []

    def flaky(m, url, **kw):
        if m.upper() == method and not seen:
            seen.append(url)
            if after_sending:
                real(m, url, **kw)  # Drive applies it; we drop the answer
            raise requests.exceptions.ReadTimeout("injected: response lost")
        return real(m, url, **kw)

    monkeypatch.setattr(drive_mod.requests, "request", flaky)
    return seen


def _named(writer, folder_id: str, name: str) -> list[dict]:
    return [c for c in writer.list_children(folder_id) if c["name"] == name]


@pytest.mark.parametrize("after_sending", [True, False], ids=["applied", "not-sent"])
def test_upload_with_a_lost_response_makes_exactly_one_file(writer, scratch, monkeypatch, after_sending):
    payload = b"a\n1\n"
    seen = _lose_first(monkeypatch, "POST", after_sending=after_sending)

    file_id = writer.upload("lost.csv", payload, scratch, "text/csv")

    assert seen, "the fault was never injected -- the test proves nothing"
    monkeypatch.undo()
    named = _named(writer, scratch, "lost.csv")
    assert [c["id"] for c in named] == [file_id], (
        f"expected exactly one lost.csv with id {file_id}, found {named}"
    )
    assert writer.download(file_id) == payload


@pytest.mark.parametrize("after_sending", [True, False], ids=["applied", "not-sent"])
def test_create_folder_with_a_lost_response_makes_exactly_one_folder(writer, scratch, monkeypatch,
                                                                    after_sending):
    seen = _lose_first(monkeypatch, "POST", after_sending=after_sending)

    folder_id = writer.create_folder("lost-folder", scratch)

    assert seen, "the fault was never injected -- the test proves nothing"
    monkeypatch.undo()
    named = _named(writer, scratch, "lost-folder")
    assert [c["id"] for c in named] == [folder_id], (
        f"expected exactly one lost-folder with id {folder_id}, found {named}"
    )


def test_a_first_attempt_conflict_is_still_an_error(writer, scratch):
    """409 means "my earlier attempt landed" only AFTER a failed attempt.

    On a first attempt it means someone else holds the id -- reusing an
    existing file's id must not be reported as a successful create.
    """
    existing = writer.upload("taken.csv", b"x\n", scratch, "text/csv")
    with pytest.raises(RuntimeError, match="409"):
        writer.upload("other.csv", b"y\n", scratch, "text/csv", file_id=existing)


def test_drive_refuses_a_reserved_id_for_a_native_conversion(writer, scratch):
    """Why upload_native cannot use the same remedy -- pinned, not assumed.

    If Drive ever starts accepting this, upload_native can reserve an id too.
    """
    reserved = writer.reserve_id()
    meta = {"id": reserved, "name": "native", "parents": [scratch],
            "mimeType": "application/vnd.google-apps.spreadsheet"}
    files = {
        "metadata": ("metadata.json", json.dumps(meta), "application/json"),
        "file": ("native", b"a,b\n1,2\n", "text/csv"),
    }
    resp = writer._request("POST", f"{drive_mod.UPLOAD_V3}/files", "create.native.reserved",
                           params={"uploadType": "multipart"}, files=files)
    assert resp.status_code == 403, resp.text[:300]
    assert "generatedIdNotSupported" in resp.text


#: Verbatim from the CI live job of 2026-10-16 (run 38860205538): Drive
#: throttles with a 403 whose reason says so, not with a 429.
_RATE_LIMITED_403 = b"""{
  "error": {
    "code": 403,
    "message": "User rate limit exceeded.",
    "errors": [
      {
        "message": "User rate limit exceeded.",
        "domain": "usageLimits",
        "reason": "userRateLimitExceeded"
      }
    ]
  }
}"""


def test_a_rate_limited_create_is_retried(writer, scratch, monkeypatch):
    """Drive's rate-limit 403 rejected the request, so a retry is safe.

    The extension classifies it as RATE_LIMIT and backs off
    (ClassifyFromReason in src/gdrive_errors.cpp); the harness must too.
    """
    real = requests.request
    throttled: list[str] = []

    def throttle_first_post(m, url, **kw):
        if m.upper() == "POST" and not throttled:
            throttled.append(url)  # never sent: Drive refused it
            resp = requests.models.Response()
            resp.status_code = 403
            resp._content = _RATE_LIMITED_403
            resp.headers["Content-Type"] = "application/json; charset=UTF-8"
            resp.url = url
            return resp
        return real(m, url, **kw)

    monkeypatch.setattr(drive_mod.requests, "request", throttle_first_post)
    folder_id = writer.create_folder("throttled-folder", scratch)

    assert throttled, "the throttle was never injected -- the test proves nothing"
    monkeypatch.undo()
    named = _named(writer, scratch, "throttled-folder")
    assert [c["id"] for c in named] == [folder_id]


def test_a_permission_403_is_not_retried(writer, scratch, monkeypatch):
    """Only the rate-limit reasons are retried; any other 403 fails at once."""
    real = requests.request
    refused: list[str] = []

    def refuse_posts(m, url, **kw):
        if m.upper() == "POST":
            refused.append(url)
            resp = requests.models.Response()
            resp.status_code = 403
            resp._content = _RATE_LIMITED_403.replace(b"userRateLimitExceeded",
                                                      b"insufficientFilePermissions")
            resp.url = url
            return resp
        return real(m, url, **kw)

    monkeypatch.setattr(drive_mod.requests, "request", refuse_posts)
    with pytest.raises(RuntimeError, match="403"):
        writer.create_folder("nope", scratch)
    assert len(refused) == 1, f"a permission 403 was retried {len(refused) - 1} time(s)"
