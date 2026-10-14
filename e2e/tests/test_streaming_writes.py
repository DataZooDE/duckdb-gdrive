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

import os
import subprocess
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
    in bytes) -- the RSS of THAT process, from wait4, not of any other child."""
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


@pytest.mark.slow
def test_large_copy_streams_in_bounded_memory(writer, scratch, duckdb_cli, gdrive_secret_sql):
    # ~370 MB of CSV from a generator that itself needs almost no memory.
    rows = 3_600_000
    uri = f"{_base(writer, scratch)}/big.csv"
    code, out, rss = _run_measured(duckdb_cli, f"""{gdrive_secret_sql}
        CALL gdrive_reset_stats();
        COPY (SELECT i, repeat('x', 100) AS pad FROM range({rows}) t(i)) TO '{uri}' (HEADER);
        SELECT metric || '=' || value FROM gdrive_stats()
         WHERE metric IN ('upload_chunks', 'upload_aborts', 'upload_buffer_peak_bytes');
        SELECT 'size=' || file_size('{uri}');
    """)
    assert code == 0, scrub(out)
    stats = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    size = int(stats["size"])
    assert size > 350 * MiB
    assert stats["upload_aborts"] == "0"
    assert int(stats["upload_chunks"]) == -(-size // CHUNK)
    # The extension never held more than one chunk...
    assert int(stats["upload_buffer_peak_bytes"]) == CHUNK
    # ...and the process as a whole stayed far below the file size. Buffering
    # the file (the old behaviour) cannot finish under `size` bytes of RSS.
    print(f"peak RSS {rss / MiB:.0f} MiB for a {size / MiB:.0f} MiB upload")
    assert rss < size / 2, f"peak RSS {rss / MiB:.0f} MiB for a {size / MiB:.0f} MiB file"

    big = writer.list_children(scratch, name="big.csv")
    assert len(big) == 1 and int(big[0]["size"]) == size


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


def test_failure_mid_stream_publishes_nothing(writer, scratch, duckdb_cli, gdrive_secret_sql):
    """The destination's folder vanishes while the upload is streaming. The COPY
    must FAIL (not report success for a file that is not there), and no file of
    that name may appear anywhere in the drive."""
    folder_id = writer.create_folder("doomed", scratch)
    name = f"x-{uuid.uuid4().hex[:8]}.csv"
    uri = f"{_base(writer, scratch)}/doomed/{name}"
    proc = subprocess.Popen([str(duckdb_cli), "-noheader", "-list"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert proc.stdin
    # ~200 MB in 256-KiB chunks: minutes of streaming, so the delete below
    # lands mid-stream. Drive shows nothing until the final chunk, so there is
    # no placeholder to wait for -- a fixed delay past startup has to do.
    proc.stdin.write(f"""{gdrive_secret_sql}
        SET gdrive_upload_chunk_bytes = 262144;
        COPY (SELECT i, repeat('x', 100) AS pad FROM range(2000000) t(i)) TO '{uri}' (HEADER, USE_TMP_FILE false);
        SELECT 'copy-returned';
    """)
    proc.stdin.close()
    output: list[str] = []
    reader = threading.Thread(target=lambda: output.append(proc.stdout.read()), daemon=True)
    reader.start()

    time.sleep(15)
    assert proc.poll() is None, "the COPY finished before the folder could be removed"
    writer.delete(folder_id, permanent=True)

    proc.wait(timeout=900)
    reader.join(timeout=30)
    text = scrub("".join(output))
    assert proc.returncode != 0, f"COPY reported success:\n{text}"
    assert "Error" in text, text
    print("mid-stream failure surfaced as:", [l for l in text.splitlines() if "Error" in l][:1])
    assert _files_named(writer, name) == []
