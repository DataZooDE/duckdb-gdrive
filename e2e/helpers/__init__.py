"""Live test harness for duckdb-gdrive.

Everything here talks to real Google Drive. There is no fake server and no
HTTP replay layer -- see CLAUDE.md, decision D-1.
"""

from .drive import Drive, DriveConfigError, credentials_available  # noqa: F401
