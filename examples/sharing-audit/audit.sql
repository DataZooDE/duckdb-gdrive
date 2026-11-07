-- Who can open what under a Drive folder -- and what is shared more widely
-- than it should be. No extension besides gdrive.
--
-- gdrive_permissions gives one row per permission on every file a glob
-- matches; read_gdrive_files gives the same files' metadata. Joined on the
-- file id, they answer the usual audit questions in plain SQL.

INSTALL gdrive FROM community;
LOAD gdrive;

-- Your gcloud login, with the Drive scope (see the README's "Authentication").
CREATE SECRET (TYPE gdrive);

CREATE OR REPLACE TABLE files AS SELECT * FROM read_gdrive_files('gdrive://Shared/**');
CREATE OR REPLACE TABLE grants AS SELECT * FROM gdrive_permissions('gdrive://Shared/**');

-- 1. General access: anyone with the link (anonymous) or the whole domain
--    (organization), and whether search can find it (discoverable).
CREATE OR REPLACE TABLE exposed AS
SELECT f.path, f.owner_email, f.modified, g.link_scope, g.role, g.discoverable, g.link_url
FROM grants g JOIN files f ON f.id = g.file_id
WHERE g.granted_by = 'link'
ORDER BY g.link_scope, f.path;

-- 2. People outside your domain with direct access. Replace example.com.
CREATE OR REPLACE TABLE external AS
SELECT f.path, g.grantee, g.grantee_email, g.role, g.expires
FROM grants g JOIN files f ON f.id = g.file_id
WHERE g.grantee_type IN ('user', 'group') AND g.grantee_email NOT LIKE '%@example.com'
ORDER BY f.path, g.grantee_email;

-- 3. Shared, but untouched for a year: candidates to unshare.
CREATE OR REPLACE TABLE stale AS
SELECT path, owner_email, modified, modified_by_email
FROM files WHERE shared AND modified < now()::TIMESTAMP - INTERVAL 1 YEAR
ORDER BY modified;

-- The report, back to Drive.
COPY exposed TO 'gdrive://Audit/exposed.csv' (FORMAT csv, HEADER);
COPY external TO 'gdrive://Audit/external.csv' (FORMAT csv, HEADER);
COPY stale TO 'gdrive://Audit/stale.csv' (FORMAT csv, HEADER);

SELECT 'link-shared' AS finding, count(*) AS files FROM exposed
UNION ALL SELECT 'external grants', count(*) FROM external
UNION ALL SELECT 'stale and shared', count(*) FROM stale;
