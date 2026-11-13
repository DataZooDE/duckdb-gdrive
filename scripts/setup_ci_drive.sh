#!/usr/bin/env bash
# One-time Google-side setup for the live test suite (plan section 2.1).
#
# ---------------------------------------------------------------------------
# WHAT CI RUNS AGAINST, AND WHY
# ---------------------------------------------------------------------------
# The CI root (GDRIVE_CI_DRIVE_ID) is a plain FOLDER in a Workspace user's
# My Drive, shared with the service account as Editor. It is not a Shared
# Drive. That works because the two identities split the work:
#
#   * the SERVICE ACCOUNT only reads. It has no Drive storage quota, so any
#     upload it would own -- anywhere outside a Shared Drive -- returns
#
#       403 "Service Accounts do not have storage quota. Leverage shared
#            drives, or use OAuth delegation instead."
#
#     (verified 2026-07-26). Folder creation succeeds, since folders use no
#     quota, which makes the failure look like a permissions problem.
#   * the DELEGATED USER (make oauth_consent) seeds, writes and sweeps. It
#     owns what it creates, against its own quota -- and in a My Drive only
#     the owner may delete, which is why the sweep runs as the user too.
#
# OPTIONAL: a Shared Drive for the DRIVE_ID test (GDRIVE_CI_SHARED_DRIVE_ID,
# test/sql/gdrive_shared_drive.test.template). Creating one needs a Workspace
# USER identity -- drives.create refuses a bare service account -- so step 5
# is manual. Until it exists, the DRIVE_ID path has no live test (CLAUDE.md,
# "Known gap").
# ---------------------------------------------------------------------------
set -euo pipefail

PROJECT="${GDRIVE_CI_PROJECT:-datazoo-gdrive-ci}"
SA_NAME="${GDRIVE_CI_SA_NAME:-gdrive-ci}"
SA_EMAIL="${SA_NAME}@${PROJECT}.iam.gserviceaccount.com"
KEY_OUT="${GDRIVE_CI_KEY_OUT:-.gcp-key.json}"

cat <<'BANNER'
================================================================================
 duckdb-gdrive :: one-time CI Drive setup
================================================================================
BANNER

step() { printf '\n\033[1m--> %s\033[0m\n' "$1"; }

step "1/6  Checking gcloud auth"
if ! gcloud auth list --filter=status:ACTIVE --format='value(account)' 2>/dev/null | grep -q .; then
    echo "No active gcloud account. Run:  gcloud auth login" >&2
    exit 1
fi
echo "active: $(gcloud auth list --filter=status:ACTIVE --format='value(account)')"

step "2/6  Project + Drive and Sheets APIs"
if ! gcloud projects describe "$PROJECT" >/dev/null 2>&1; then
    echo "Project '$PROJECT' not found."
    echo "Create it, or point at an existing one with GDRIVE_CI_PROJECT=<id>."
    exit 1
fi
# The Sheets API backs read_sheet_range and COPY ... (FORMAT sheet_range);
# without it those tests fail with 403 SERVICE_DISABLED. Both identities bill
# to this project: the service account it owns, and the delegated user's
# OAuth client, which should be created here too.
gcloud services enable drive.googleapis.com sheets.googleapis.com --project="$PROJECT"
echo "drive.googleapis.com and sheets.googleapis.com enabled on $PROJECT"

step "3/6  Service account"
if ! gcloud iam service-accounts describe "$SA_EMAIL" --project="$PROJECT" >/dev/null 2>&1; then
    gcloud iam service-accounts create "$SA_NAME" \
        --project="$PROJECT" \
        --display-name="duckdb-gdrive CI"
fi
echo "service account: $SA_EMAIL"

if [[ ! -f "$KEY_OUT" ]]; then
    gcloud iam service-accounts keys create "$KEY_OUT" \
        --iam-account="$SA_EMAIL" --project="$PROJECT"
    chmod 600 "$KEY_OUT"
    echo "key written to $KEY_OUT (0600, gitignored)"
else
    echo "key already present at $KEY_OUT -- not overwriting"
fi

step "4/6  MANUAL: the CI root folder"
cat <<MANUAL

  As the Workspace user who will also be the delegated user (make
  oauth_consent signs in as the same account):

    1. In My Drive, create a folder, e.g.  duckdb-gdrive
    2. Share it with

           $SA_EMAIL

       as **Editor** (the SA reads; it never needs to delete).
    3. Copy the folder id from the URL:
           https://drive.google.com/drive/folders/<THIS_IS_THE_FOLDER_ID>

MANUAL

read -r -p "  Paste the folder id (or press Enter to skip): " DRIVE_ID || true

step "5/6  OPTIONAL, MANUAL: a Shared Drive for the DRIVE_ID test"
cat <<MANUAL

  Only needed for test/sql/gdrive_shared_drive.test.template. Creating a
  Shared Drive requires a Google Workspace *user*; a service account cannot.

    1. Open https://drive.google.com/drive/shared-drives
    2. New  ->  name it:  duckdb-gdrive-ci
    3. Manage members  ->  add:
           $SA_EMAIL             as **Content manager** (reads, and writes:
                                  the test proves an SA CAN write here)
           <the delegated user>  as **Manager** (writes, and teardown
                                  permanently deletes the run folder)
       If the delegated user created the drive it is already a Manager.
    4. Copy the drive id (starts 0A) from the URL:
           https://drive.google.com/drive/folders/<THIS_IS_THE_DRIVE_ID>

MANUAL

read -r -p "  Paste the Shared Drive id (or press Enter to skip): " SHARED_DRIVE_ID || true

step "6/6  Writing .env.gdrive"
if [[ -n "${DRIVE_ID:-}" ]]; then
    cat > .env.gdrive <<ENV
# Generated by scripts/setup_ci_drive.sh -- NEVER COMMIT THIS FILE.
GDRIVE_CI_SA_KEY_FILE=$KEY_OUT
GDRIVE_CI_DRIVE_ID=$DRIVE_ID
GDRIVE_CI_SHARED_DRIVE_ID=${SHARED_DRIVE_ID:-}
ENV
    chmod 600 .env.gdrive
    echo "wrote .env.gdrive"
    echo ""
    echo "Next:"
    echo "  make oauth_consent     # the delegated user's refresh token"
    echo "  make seed_fixtures     # upload the permanent fixture tree"
    echo "  make e2e               # prove the harness works end to end"
else
    echo "skipped -- create .env.gdrive from .env.gdrive.example by hand."
fi

step "For CI"
cat <<CI
  Add these GitHub Actions secrets to DataZooDE/duckdb-gdrive:

    GDRIVE_CI_SA_KEY_B64   = \$(base64 -w0 $KEY_OUT)
    GDRIVE_CI_DRIVE_ID          = <the CI root folder id>
    GDRIVE_OAUTH_CLIENT_ID / GDRIVE_OAUTH_CLIENT_SECRET / GDRIVE_USER_REFRESH_TOKEN
                                = from make oauth_consent
    GDRIVE_CI_SHARED_DRIVE_ID   = <the Shared Drive id>   (optional, step 5)

  Rotate the local key file if it was ever shared.
CI
