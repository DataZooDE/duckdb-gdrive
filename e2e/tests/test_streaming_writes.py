"""Streaming writes: a large write is uploaded chunk by chunk through a Drive
resumable session while it is being written, so it needs ~one chunk of memory
instead of the whole file, and Drive publishes the file only when the final
chunk lands on close.

Memory is proven twice over: by the extension's own high-water mark
(gdrive_stats() 'upload_buffer_peak_bytes') and by the duckdb process's peak
RSS as the kernel reports it (wait4), against a file several times larger.
Shared in shape with duckdb-sharepoint's e2e/tests/test_streaming_writes.py.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import threading
import time
import uuid

import pytest

from conftest import scrub

pytestmark = pytest.mark.live

MiB = 1024 * 1024
CHUNK = 16 * MiB  # the default gdrive_upload_chunk_bytes


def _base(writer, scratch_id: str) -> str:
    return f"gdrive://scratch/{writer.get_metadata(scratch_id)['name']}"


def _run_measured(duckdb_cli, statements: str) -> tuple[int, str, int]:
    """Run SQL in a fresh duckdb process; return (exit code, output, peak RSS
    in bytes) -- the RSS of THAT process, from wait4, not of any other child.
    Linux only: ru_maxrss is KiB there (bytes on macOS)."""
    proc = subprocess.Popen([str(duckdb_cli), "-noheader", "-list"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert proc.stdin and proc.stdout
    proc.stdin.write(statements)
    proc.stdin.close()
    out = proc.stdout.read()
    _, status, usage = os.wait4(proc.pid, 0)
    proc.returncode = os.waitstatus_to_exitcode(status)
    return proc.returncode, out, usage.ru_maxrss * 1024  # Linux reports KiB


def _files_named(drive, name: str) -> list[dict]:
    """Every non-trashed file with this exact name, anywhere in the drive."""
    params = {"q": f"name = '{name}' and trashed = false", "fields": "files(id, name, size)"}
    if drive.is_shared_drive:
        params.update({"corpora": "drive", "driveId": drive.drive_id})
    resp = drive._request("GET", "https://www.googleapis.com/drive/v3/files", "files.list", params=params)
    return drive._ok(resp, "files.list").get("files", [])


def _copy_measured(duckdb_cli, secret_sql: str, uri: str, rows: int) -> tuple[dict, int]:
    code, out, rss = _run_measured(duckdb_cli, f"""{secret_sql}
        SET threads = 4;
        CALL gdrive_reset_stats();
        COPY (SELECT i, repeat('x', 100) AS pad FROM range({rows}) t(i)) TO '{uri}' (HEADER);
        SELECT metric || '=' || value FROM gdrive_stats()
         WHERE metric IN ('upload_chunks', 'upload_aborts', 'upload_buffer_peak_bytes', 'retries');
        SELECT 'size=' || file_size('{uri}');
    """)
    assert code == 0, scrub(out)
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line), rss


@pytest.mark.slow
@pytest.mark.skipif(sys.platform != "linux", reason="ru_maxrss is KiB only on Linux")
def test_memory_does_not_grow_with_file_size(writer, scratch, duckdb_cli, gdrive_secret_sql):
    """The property, not a threshold: tripling the file must not grow peak RSS
    by anything like the extra bytes. Buffering the file (the old behaviour)
    grows it by at least the difference; streaming by roughly nothing."""
    base = _base(writer, scratch)
    small, rss_small = _copy_measured(duckdb_cli, gdrive_secret_sql, f"{base}/small.csv", 1_200_000)
    big, rss_big = _copy_measured(duckdb_cli, gdrive_secret_sql, f"{base}/big.csv", 3_600_000)
    size_small, size_big = int(small["size"]), int(big["size"])
    assert size_big > 350 * MiB and size_big > 2.5 * size_small
    for stats, size in ((small, size_small), (big, size_big)):
        assert stats["upload_aborts"] == "0"
        assert int(stats["upload_buffer_peak_bytes"]) == CHUNK
        chunks = int(stats["upload_chunks"])
        assert chunks == -(-size // CHUNK) if stats["retries"] == "0" else chunks >= -(-size // CHUNK)
    growth = rss_big - rss_small
    print(f"peak RSS {rss_small / MiB:.0f} MiB at {size_small / MiB:.0f} MiB, "
          f"{rss_big / MiB:.0f} MiB at {size_big / MiB:.0f} MiB")
    assert growth < (size_big - size_small) / 4, f"RSS grew {growth / MiB:.0f} MiB"
    big_files = writer.list_children(scratch, name="big.csv")
    assert len(big_files) == 1 and int(big_files[0]["size"]) == size_big


def test_streamed_bytes_round_trip_exactly(writer, scratch, sql, gdrive_secret_sql, tmp_path):
    # Several chunks at the 256-KiB minimum, compared byte for byte with the
    # same COPY to local disk.
    uri = f"{_base(writer, scratch)}/s.csv"
    local = tmp_path / "s.csv"
    out = sql(f"""{gdrive_secret_sql}
        SET gdrive_upload_chunk_bytes = 262144;
        CALL gdrive_reset_stats();
        COPY (SELECT i, md5(i::VARCHAR) AS h FROM range(60000) t(i)) TO '{uri}' (HEADER);
        COPY (SELECT i, md5(i::VARCHAR) AS h FROM range(60000) t(i)) TO '{local}' (HEADER);
        SELECT (SELECT value FROM gdrive_stats() WHERE metric = 'upload_chunks') || '|' ||
               (SELECT md5(content) FROM read_blob('{uri}')) || '|' ||
               (SELECT md5(content) FROM read_blob('{local}'));""")
    chunks, remote_md5, local_md5 = out.splitlines()[-1].split("|")
    assert int(chunks) == -(-local.stat().st_size // 262144) and int(chunks) > 3
    assert remote_md5 == local_md5


def _spawn_traced(duckdb_cli, statements: str, trace: str) -> tuple[subprocess.Popen, list]:
    env = dict(os.environ, GDRIVE_TRACE_FILE=trace)
    proc = subprocess.Popen([str(duckdb_cli), "-noheader", "-list"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    assert proc.stdin and proc.stdout
    proc.stdin.write(statements)
    proc.stdin.close()
    output: list[str] = []
    threading.Thread(target=lambda: output.append(proc.stdout.read()), daemon=True).start()
    return proc, output


def _spans(trace: str, kind: str) -> list[dict]:
    try:
        lines = open(trace).read().splitlines()
    except FileNotFoundError:
        return []
    out = []
    for line in lines:
        try:
            rec = json.loads(line)
        except ValueError:
            continue  # a line still being written
        if rec.get("kind") == kind:
            out.append(rec)
    return out


def _wait_for_chunks(proc, trace: str, n: int, timeout: float = 180) -> None:
    """Block until the upload has sent `n` acknowledged non-final chunks
    (trace spans `upload.chunk` with status 308): genuinely mid-stream."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if len([sp for sp in _spans(trace, "upload.chunk") if sp["status"] == 308]) >= n:
            return
        assert proc.poll() is None, "the COPY ended before it was observed mid-stream"
        time.sleep(0.2)
    raise AssertionError("the upload never got going")


def _finish(proc, output: list, timeout: float = 900) -> str:
    try:
        proc.wait(timeout=timeout)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=30)
    time.sleep(0.5)  # let the reader thread collect the tail
    return scrub("".join(output))


def test_failure_mid_stream_publishes_nothing(writer, scratch, duckdb_cli, gdrive_secret_sql, tmp_path):
    """Mid-stream (3 chunks acknowledged, per the trace) the file is NOT
    visible; then the destination folder vanishes. The COPY must FAIL -- not
    report success for a file that is not there -- and no file of that name
    may appear anywhere in the drive."""
    folder_id = writer.create_folder("doomed", scratch)
    name = f"x-{uuid.uuid4().hex[:8]}.csv"
    uri = f"{_base(writer, scratch)}/doomed/{name}"
    trace = str(tmp_path / "trace.jsonl")
    proc, output = _spawn_traced(duckdb_cli, f"""{gdrive_secret_sql}
        SET gdrive_upload_chunk_bytes = 262144;
        COPY (SELECT i, repeat('x', 100) AS pad FROM range(2000000) t(i)) TO '{uri}' (HEADER, USE_TMP_FILE false);
        SELECT 'copy-returned';
    """, trace)
    try:
        _wait_for_chunks(proc, trace, 3)
        assert writer.list_children(folder_id, name=name) == [], "visible before the final chunk"
        writer.delete(folder_id, permanent=True)
    finally:
        text = _finish(proc, output)
    assert proc.returncode != 0, f"COPY reported success:\n{text}"
    assert "Error" in text, text
    print("mid-stream failure surfaced as:", [l for l in text.splitlines() if "Error" in l][:1])
    assert _files_named(writer, name) == []


def test_streamed_overwrite_keeps_old_content_until_done(writer, scratch, duckdb_cli, gdrive_secret_sql, tmp_path):
    """Overwriting an existing file: mid-stream it still has its OLD bytes;
    afterwards it has the new ones under the SAME file id (files.update)."""
    old = b"old content\n" * 1000
    file_id = writer.upload("over.csv", old, scratch, "text/csv")
    uri = f"{_base(writer, scratch)}/over.csv"
    trace = str(tmp_path / "trace.jsonl")
    proc, output = _spawn_traced(duckdb_cli, f"""{gdrive_secret_sql}
        SET gdrive_upload_chunk_bytes = 262144;
        SELECT write_blob('{uri}', (SELECT string_agg(repeat('n', 99), chr(10)) FROM range(400000))::BLOB);
    """, trace)
    try:
        _wait_for_chunks(proc, trace, 3)
        assert hashlib.md5(writer.download(file_id)).hexdigest() == hashlib.md5(old).hexdigest()
    finally:
        text = _finish(proc, output)
    assert proc.returncode == 0, text
    after = writer.list_children(scratch, name="over.csv")
    assert [f["id"] for f in after] == [file_id]
    assert int(after[0]["size"]) == 400000 * 100 - 1


def test_failed_query_cancels_the_session(writer, scratch, duckdb_cli, gdrive_secret_sql, tmp_path):
    """A query error mid-COPY drops the handle; the session is cancelled and
    Drive confirms it (499 on the upload.cancel span)."""
    trace = str(tmp_path / "trace.jsonl")
    uri = f"{_base(writer, scratch)}/boom.csv"
    proc, output = _spawn_traced(duckdb_cli, f"""{gdrive_secret_sql}
        SET threads = 1;
        SET gdrive_upload_chunk_bytes = 262144;
        COPY (SELECT i, CASE WHEN i < 90000 THEN md5(i::VARCHAR) ELSE error('boom mid-stream') END AS h
              FROM range(100000) t(i)) TO '{uri}' (HEADER);
    """, trace)
    text = _finish(proc, output)
    assert "boom mid-stream" in text, text
    assert len([sp for sp in _spans(trace, "upload.chunk") if sp["status"] == 308]) >= 3
    assert [sp["status"] for sp in _spans(trace, "upload.cancel")] == [499]
    assert writer.list_children(scratch, name="boom.csv") == []
