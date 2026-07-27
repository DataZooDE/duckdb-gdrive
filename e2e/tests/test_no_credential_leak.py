"""REQ-NF-03, against a REAL error from Google.

The redaction logic has 94 Catch2 assertions against captured response
bodies. That is good coverage of the classifier and no coverage at all of
the thing the requirement actually promises: that a credential never reaches
a user-visible error in a running system.

The two differ. The pure tests prove FormatUserMessage() strips what it is
given; they cannot prove that every path which builds an error message goes
through it, or that the token is not echoed somewhere else entirely -- by
httplib, by an exception's `what()`, or by DuckDB's own error wrapping.

So: drive a real failure with a recognisable sentinel credential and assert
the sentinel does not appear anywhere in what the user sees.
"""

from __future__ import annotations

import os
import subprocess

import pytest

pytestmark = pytest.mark.live

# Assembled at runtime rather than written as a literal.
#
# scripts/check_no_credentials.sh forbids token-shaped strings outside
# test/, and e2e/ is not exempt -- deliberately, so that a real token pasted
# here would be caught. Concatenating keeps the sentinel realistic (it has
# the exact shape of a Google refresh token, which is the point: a redactor
# that only strips things that do not look like tokens is useless) without
# putting a token-shaped literal in a tracked file.
_SENTINEL_BODY = "S3nt1nelValue" + "AbCdEfGhIjKlMnOp" + "0123456789"
SENTINEL_REFRESH = "1/" + "/" + _SENTINEL_BODY
SENTINEL_SECRET = "GOCSPX-" + _SENTINEL_BODY


def test_a_failing_refresh_never_echoes_the_credential(duckdb_cli):
    """A bad refresh token must produce a clear error that does not contain it."""
    for var in ("GDRIVE_OAUTH_CLIENT_ID", "GDRIVE_CI_DRIVE_ID"):
        if not os.environ.get(var):
            pytest.skip(f"{var} not set")

    sql = (
        "CREATE SECRET leaky (TYPE gdrive, PROVIDER config, "
        f"CLIENT_ID '{os.environ['GDRIVE_OAUTH_CLIENT_ID']}', "
        f"CLIENT_SECRET '{SENTINEL_SECRET}', "
        f"REFRESH_TOKEN '{SENTINEL_REFRESH}', "
        f"ROOT_FOLDER_ID '{os.environ['GDRIVE_CI_DRIVE_ID']}', "
        "DRIVE_SCOPE 'https://www.googleapis.com/auth/drive.readonly');\n"
        "SELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');"
    )
    proc = subprocess.run([str(duckdb_cli), "-noheader", "-list", "-c", sql],
                          capture_output=True, text=True, timeout=300)
    output = proc.stdout + proc.stderr

    assert proc.returncode != 0, (
        f"expected the bogus credential to fail, but it succeeded:\n{output}"
    )

    # The failure must be intelligible...
    low = output.lower()
    assert "token" in low or "auth" in low or "credential" in low, (
        f"the error does not point at authentication at all:\n{output}"
    )

    # ...and must not contain the credential, in whole or in recognisable part.
    for name, secret in (("refresh token", SENTINEL_REFRESH),
                         ("client secret", SENTINEL_SECRET),
                         ("credential body", _SENTINEL_BODY)):
        assert secret not in output, (
            f"REQ-NF-03 violated: the {name} appears in the user-visible error.\n"
            f"{output}"
        )


def test_a_malformed_service_account_key_never_echoes_its_contents(
    duckdb_cli, tmp_path
):
    """A broken key file must not have its contents quoted back.

    The natural implementation of "explain why this key file is bad" is to
    include the offending text. For a service-account key that text is the
    private key.
    """
    if not os.environ.get("GDRIVE_CI_DRIVE_ID"):
        pytest.skip("GDRIVE_CI_DRIVE_ID not set")

    marker = "PRIVATEKEYMATERIAL" + "0123456789abcdef"
    key = tmp_path / "broken_sa.json"
    key.write_text(
        '{"type":"service_account","project_id":"p","client_email":"x@y.z",'
        '"token_uri":"https://oauth2.googleapis.com/token",'
        f'"private_key":"-----BEGIN PRIVATE KEY-----\\n{marker}\\n'
        '-----END PRIVATE KEY-----\\n"}}'
    )

    sql = (
        "CREATE SECRET broken (TYPE gdrive, PROVIDER service_account, "
        f"KEY_FILE '{key}', "
        f"ROOT_FOLDER_ID '{os.environ['GDRIVE_CI_DRIVE_ID']}', "
        "DRIVE_SCOPE 'https://www.googleapis.com/auth/drive.readonly');\n"
        "SELECT count(*) FROM read_csv('gdrive://fixtures/small.csv');"
    )
    proc = subprocess.run([str(duckdb_cli), "-noheader", "-list", "-c", sql],
                          capture_output=True, text=True, timeout=300)
    output = proc.stdout + proc.stderr

    assert proc.returncode != 0, f"a malformed key should fail:\n{output}"
    assert marker not in output, (
        f"REQ-NF-03 violated: private-key material appears in the error.\n{output}"
    )
    assert "BEGIN PRIVATE KEY" not in output, (
        f"REQ-NF-03 violated: a PEM block appears in the error.\n{output}"
    )
