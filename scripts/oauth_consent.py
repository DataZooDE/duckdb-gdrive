#!/usr/bin/env python3
"""One-time interactive Google consent, yielding a stored refresh token.

Why this exists: a service account has no Drive storage quota, so it cannot
create the test fixtures (Google returns 403 storageQuotaExceeded outside a
Shared Drive). Delegating to a real user lets the harness seed fixtures under
that user's quota, into a folder the service account can then READ -- which
unblocks the whole read path without needing a Shared Drive.

It also exercises the authorization_code path REQ-F-05 requires, including the
two parameters Google will not issue a refresh token without.

Run:  uv run --with requests python scripts/oauth_consent.py

Writes GDRIVE_USER_REFRESH_TOKEN / CLIENT_ID / CLIENT_SECRET into .env.gdrive,
which is gitignored and enforced by scripts/check_no_credentials.sh.
"""

from __future__ import annotations

import http.server
import json
import os
import secrets
import sys
import threading
import urllib.parse
from pathlib import Path

import requests

REPO_ROOT = Path(__file__).resolve().parents[1]
CLIENT_JSON = Path(
    os.environ.get(
        "GDRIVE_OAUTH_CLIENT_JSON",
        "/home/jr/Projects/datazoo/quack-oauth/"
        "client_secret_1025084330164-m20tki6eqknm8j12brhto315ioogu555.apps.googleusercontent.com.json",
    )
)

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
SCOPE = "https://www.googleapis.com/auth/drive"
REDIRECT = "http://localhost:8000/oauth/callback"
PORT = 8000

_result: dict = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/oauth/callback":
            self.send_response(404)
            self.end_headers()
            return
        params = urllib.parse.parse_qs(parsed.query)
        _result["code"] = params.get("code", [None])[0]
        _result["state"] = params.get("state", [None])[0]
        _result["error"] = params.get("error", [None])[0]
        body = (b"<h2>duckdb-gdrive: consent received.</h2>"
                b"<p>You can close this tab and return to the terminal.</p>")
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # silence the default stderr logging
        pass


def main() -> int:
    if not CLIENT_JSON.exists():
        print(f"OAuth client JSON not found: {CLIENT_JSON}", file=sys.stderr)
        return 2
    doc = json.loads(CLIENT_JSON.read_text())
    cfg = doc.get("web") or doc.get("installed")
    client_id, client_secret = cfg["client_id"], cfg["client_secret"]

    state = secrets.token_urlsafe(24)
    query = urllib.parse.urlencode({
        "client_id": client_id,
        "redirect_uri": REDIRECT,
        "response_type": "code",
        "scope": SCOPE,
        "state": state,
        # Google issues NO refresh token without both of these. This is the
        # one genuinely Google-shaped requirement in the whole OAuth flow
        # (HLD section 5.3) and the reason datazoo-oauth2 grew a generic
        # extra_auth_params map rather than a Google branch.
        "access_type": "offline",
        "prompt": "consent",
    })

    server = http.server.HTTPServer(("localhost", PORT), Handler)
    threading.Thread(target=server.handle_request, daemon=True).start()

    # Also written to a file: stdout is easily buffered away when this runs
    # detached, and the URL is the one thing a human must actually see.
    url = f"{AUTH_URL}?{query}"
    url_file = REPO_ROOT / ".oauth_consent_url.txt"
    url_file.write_text(url + "\n")

    print("\n" + "=" * 78, flush=True)
    print("Open this URL in your browser and grant access:\n", flush=True)
    print(url + "\n", flush=True)
    print("=" * 78 + "\n", flush=True)
    print(f"Waiting for the callback on {REDIRECT} ...")

    server.socket.settimeout(300)
    for _ in range(300):
        if _result:
            break
        threading.Event().wait(1)

    if not _result:
        print("timed out waiting for consent", file=sys.stderr)
        return 1
    if _result.get("error"):
        print(f"consent denied: {_result['error']}", file=sys.stderr)
        return 1
    if _result.get("state") != state:
        # CSRF check. Not ceremony: without it a foreign callback could hand
        # us a code minted for someone else's session.
        print("state mismatch -- possible CSRF, aborting", file=sys.stderr)
        return 1

    resp = requests.post(TOKEN_URL, data={
        "code": _result["code"],
        "client_id": client_id,
        "client_secret": client_secret,
        "redirect_uri": REDIRECT,
        "grant_type": "authorization_code",
    }, timeout=60)
    if resp.status_code != 200:
        print(f"token exchange failed: {resp.status_code} {resp.text[:300]}", file=sys.stderr)
        return 1
    tok = resp.json()
    refresh = tok.get("refresh_token")
    if not refresh:
        print("no refresh_token returned -- access_type=offline and prompt=consent "
              "are both required; revoke prior consent and retry.", file=sys.stderr)
        return 1

    env = REPO_ROOT / ".env.gdrive"
    existing = env.read_text() if env.exists() else ""
    keep = [ln for ln in existing.splitlines()
            if not ln.startswith(("GDRIVE_USER_", "GDRIVE_OAUTH_"))]
    keep += [
        f"GDRIVE_OAUTH_CLIENT_ID={client_id}",
        f"GDRIVE_OAUTH_CLIENT_SECRET={client_secret}",
        f"GDRIVE_USER_REFRESH_TOKEN={refresh}",
    ]
    env.write_text("\n".join(keep) + "\n")
    env.chmod(0o600)

    print("\nConsent complete. Refresh token stored in .env.gdrive (0600, gitignored).")
    print("The token itself is deliberately not printed.")
    print("\nNext:  make seed_fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
