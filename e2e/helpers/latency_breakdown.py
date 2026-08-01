"""Where the time inside a Drive request actually goes.

    make latency_breakdown

`helpers.latency` answers "what does a request cost". This answers "which part
of that cost is ours". The distinction decides what is worth optimising: if the
residual after connection setup is Google's service time, the only remaining
lever is fewer requests; if it is body transfer, the block cache is fetching
more than it needs.

WHY RAW SOCKETS. `requests` cannot report the phases -- by the time it hands
back a response the handshake, the server's think time and the transfer are one
number. libcurl exposes them via NAMELOOKUP/CONNECT/APPCONNECT/STARTTRANSFER,
but pycurl is a compiled dependency this repo does not otherwise need. The
stdlib gives all six directly, at the cost of speaking HTTP/1.1 by hand, which
is about eighty lines and is done below.

PHASES

    dns       getaddrinfo
    tcp       socket.connect
    tls       ssl handshake (wrap_socket)
    send      writing the request bytes
    ttfb      last byte sent -> first byte of the response  == server think time
    body      first byte -> last byte of the body           == transfer

`ttfb` is the interesting one. It is the part no amount of client engineering
can remove, so it is the floor that request-count reduction has to amortise.

COLD vs WARM. Every shape is measured twice: on a new connection (paying dns,
tcp and tls) and on a reused one (where those are zero by construction). The
extension holds a thread_local keep-alive connection, so warm is what a running
query sees -- but publishing only the warm number hides how much the first
request of a query costs, which is exactly what a cold Parquet scan is made of.

Output is JSON on stdout for the figure generator, and a table on stderr for a
human. Nothing here writes to Drive.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import ssl
import statistics
import sys
import time
from urllib.parse import urlencode

import google.auth.transport.requests as gauth_requests

from . import fixtures as fx
from .drive import Drive, DriveConfigError

HOST = "www.googleapis.com"
PORT = 443
API = "/drive/v3"
SHARED = {"supportsAllDrives": "true", "includeItemsFromAllDrives": "true"}

PHASES = ("dns", "tcp", "tls", "send", "ttfb", "body")


class Conn:
    """One HTTPS connection, with the setup phases timed.

    Deliberately not a context manager over a single request: the whole point
    is to issue a second request on the same socket and see the difference.
    """

    def __init__(self, ctx: ssl.SSLContext) -> None:
        t0 = time.perf_counter()
        addr = socket.getaddrinfo(HOST, PORT, socket.AF_INET, socket.SOCK_STREAM)[0][4]
        t1 = time.perf_counter()

        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw.settimeout(300)
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        raw.connect(addr)
        t2 = time.perf_counter()

        self.sock = ctx.wrap_socket(raw, server_hostname=HOST)
        t3 = time.perf_counter()

        self.setup = {"dns": t1 - t0, "tcp": t2 - t1, "tls": t3 - t2}
        self.reader = self.sock.makefile("rb")

    def close(self) -> None:
        try:
            self.reader.close()
            self.sock.close()
        except OSError:
            pass

    def request(self, path: str, token: str, extra: dict | None = None) -> dict:
        """Issue one GET and return per-phase seconds plus the body length."""
        headers = {
            "Host": HOST,
            "Authorization": f"Bearer {token}",
            # identity, so that `body` measures the bytes the extension would
            # actually receive rather than a gzip stream it never asks for,
            # and so Content-Length is present and trustworthy.
            "Accept-Encoding": "identity",
            "Connection": "keep-alive",
            "User-Agent": "duckdb-gdrive-latency-breakdown/1",
        }
        headers.update(extra or {})
        blob = f"GET {path} HTTP/1.1\r\n" + "".join(
            f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n"

        t0 = time.perf_counter()
        self.sock.sendall(blob.encode("ascii"))
        t1 = time.perf_counter()

        status_line = self.reader.readline()
        t2 = time.perf_counter()          # first byte of the response
        if not status_line:
            raise ConnectionError("server closed the connection before responding")
        status = int(status_line.split()[1])

        resp_headers = {}
        while True:
            line = self.reader.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            k, _, v = line.decode("iso-8859-1").partition(":")
            resp_headers[k.strip().lower()] = v.strip()

        n = self._read_body(resp_headers)
        t3 = time.perf_counter()

        return {
            "status": status,
            "bytes": n,
            "send": t1 - t0,
            "ttfb": t2 - t1,
            "body": t3 - t2,
        }

    def _read_body(self, headers: dict) -> int:
        if headers.get("transfer-encoding", "").lower() == "chunked":
            total = 0
            while True:
                size = int(self.reader.readline().split(b";")[0], 16)
                if size == 0:
                    self.reader.readline()      # trailing CRLF
                    return total
                total += len(self.reader.read(size))
                self.reader.read(2)             # CRLF after each chunk
        length = int(headers.get("content-length", 0))
        return len(self.reader.read(length)) if length else 0


def measure(ctx, token: str, path: str, extra: dict | None, repeats: int) -> dict:
    """Return cold and warm phase medians for one request shape.

    Cold: a fresh connection per sample, so dns/tcp/tls are real.
    Warm: one connection, one untimed priming request, then `repeats` more --
    which is what the extension's thread_local client does after its first
    call.
    """
    cold = []
    for _ in range(repeats):
        c = Conn(ctx)
        try:
            r = c.request(path, token, extra)
            _check(r, path)
            cold.append({**c.setup, **{k: r[k] for k in ("send", "ttfb", "body")},
                         "bytes": r["bytes"]})
        finally:
            c.close()

    warm = []
    c = Conn(ctx)
    try:
        _check(c.request(path, token, extra), path)   # prime, untimed
        for _ in range(repeats):
            r = c.request(path, token, extra)
            _check(r, path)
            warm.append({"dns": 0.0, "tcp": 0.0, "tls": 0.0,
                         **{k: r[k] for k in ("send", "ttfb", "body")},
                         "bytes": r["bytes"]})
    finally:
        c.close()

    return {"cold": _median_phases(cold), "warm": _median_phases(warm),
            "bytes": cold[0]["bytes"], "n": repeats}


def _check(r: dict, path: str) -> None:
    # 206 is the expected answer to a Range request; 200 means Drive ignored
    # the header and sent everything, which is a real behaviour the extension
    # normalises -- here it would silently turn a 1 KiB row into an 87 MB one,
    # so it must not pass unnoticed.
    if r["status"] not in (200, 206):
        raise RuntimeError(f"HTTP {r['status']} for {path.split('?')[0]}")


def _median_phases(samples: list[dict]) -> dict:
    out = {p: round(statistics.median(s[p] for s in samples) * 1000, 1) for p in PHASES}
    out["total"] = round(sum(out.values()), 1)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--json", default="", help="also write raw results here")
    args = ap.parse_args()

    try:
        drive = Drive.from_env()
    except DriveConfigError as e:
        print(f"SKIP: {e}", file=sys.stderr)
        return 0

    creds = drive._creds
    if not creds.valid:
        creds.refresh(gauth_requests.Request())
    token = creds.token

    root = os.environ["GDRIVE_CI_DRIVE_ID"]
    small_id = drive.resolve_path(f"{fx.FIXTURES_ROOT}/small.csv")
    try:
        big_id = drive.resolve_path("fixtures/wide.parquet")
    except Exception as e:                                   # noqa: BLE001
        print(f"NOTE: no fixtures/wide.parquet ({e}); ranged shapes skipped.",
              file=sys.stderr)
        big_id = None

    q = urlencode({**SHARED, "q": f"'{root}' in parents", "fields": "files(id,name)"})
    shapes = [
        ("files.list", f"{API}/files?{q}", None, args.repeats),
        ("files.get",
         f"{API}/files/{small_id}?" + urlencode(
             {**SHARED, "fields": "id,name,size,mimeType,headRevisionId"}),
         None, args.repeats),
        ("alt=media, whole small file",
         f"{API}/files/{small_id}?" + urlencode({**SHARED, "alt": "media"}),
         None, args.repeats),
    ]
    if big_id:
        media = f"{API}/files/{big_id}?" + urlencode({**SHARED, "alt": "media"})
        for label, upper in (("1 KiB", 1023), ("1 MiB", 1048575),
                             ("16 MiB", 16777215)):
            shapes.append((f"alt=media, {label} range",
                           media, {"Range": f"bytes=0-{upper}"}, args.repeats))

    ctx = ssl.create_default_context()
    results = {}
    for label, path, extra, repeats in shapes:
        print(f"  measuring {label} ...", file=sys.stderr, flush=True)
        try:
            results[label] = measure(ctx, token, path, extra, repeats)
        except Exception as e:                               # noqa: BLE001
            print(f"  FAILED {label}: {e}", file=sys.stderr)

    hdr = f"{'shape':<28} {'conn':<5} " + " ".join(f"{p:>7}" for p in PHASES) + f"{'total':>9}"
    print("\n" + hdr, file=sys.stderr)
    print("-" * len(hdr), file=sys.stderr)
    for label, r in results.items():
        for kind in ("cold", "warm"):
            row = r[kind]
            print(f"{label:<28} {kind:<5} "
                  + " ".join(f"{row[p]:>7.1f}" for p in PHASES)
                  + f"{row['total']:>9.1f}", file=sys.stderr)

    blob = json.dumps(results, indent=2)
    if args.json:
        with open(args.json, "w") as fh:
            fh.write(blob + "\n")
        print(f"\nwrote {args.json}", file=sys.stderr)
    print(blob)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
