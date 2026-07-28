PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=gdrive
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---------------------------------------------------------------------------
# extension-ci-tools defines `test: test_release`, which runs only the SQL
# suite. That made `make test` green while the Catch2 pure-logic binary was
# never executed -- precisely the false green the two-layer policy exists to
# prevent, and worse because it was silent.
#
# Override so the headline target runs BOTH layers. test_live stays separate:
# it needs credentials and must fail loudly without them.
# ---------------------------------------------------------------------------
.PHONY: test
test: unit_test test_release

# ---------------------------------------------------------------------------
# Catch2 unit tests -- pure logic, no DuckDB linkage, no network, no
# credentials. Always runnable by anyone who can build. See
# docs/implementation-plan.md section 1.
# ---------------------------------------------------------------------------
.PHONY: unit_test
unit_test: release
	@if [ ! -x build/release/test/gdrive_unit_tests ]; then \
		echo "build/release/test/gdrive_unit_tests not found -- check Catch2 in vcpkg.json"; \
		exit 1; \
	fi
	./build/release/test/gdrive_unit_tests --reporter console::out=-

# ---------------------------------------------------------------------------
# Live SQL tests against REAL Google Drive. There is no fake and no replay
# (decision D-1). Without credentials these skip cleanly via `require-env`,
# so `make test` stays green for a developer who has none.
#
# test_live is the same suite but FAILS LOUDLY when credentials are absent --
# use it in CI where a silent skip would be a false green.
# ---------------------------------------------------------------------------
.PHONY: test_live
test_live: release
	@./scripts/run_live_tests.sh

# ---------------------------------------------------------------------------
# Fixture management against the CI Shared Drive.
#   seed_fixtures  -- idempotent upload of the permanent read-only fixtures
#   sweep_orphans  -- delete /scratch/run-* folders older than 24h (crashed
#                     runs leak them)
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# One-time interactive Google consent. Prints a URL, waits 5 minutes for the
# loopback callback, stores the refresh token in .env.gdrive (0600).
#
# Needed because a service account has NO Drive storage quota and cannot
# create the fixtures at all outside a Shared Drive. Seeding therefore runs
# as a delegated user; the read tests still run as the service account
# against the same folder.
# ---------------------------------------------------------------------------
.PHONY: oauth_consent
oauth_consent:
	@uv run --with requests python -u scripts/oauth_consent.py

.PHONY: seed_fixtures
seed_fixtures:
	@cd e2e && uv run python -m helpers.seed

.PHONY: sweep_orphans
sweep_orphans:
	@cd e2e && uv run python -m helpers.sweep

# ---------------------------------------------------------------------------
# Python E2E harness: fixture provisioning/teardown, API-call-count
# assertions, the headless interactive-OAuth flow, and write round trips --
# the things SQLLogicTest cannot express. Lives outside test/ so DuckDB's
# unittest scanner does not walk the venv.
# ---------------------------------------------------------------------------
.PHONY: e2e
e2e: release
	@cd e2e && uv sync --frozen && uv run pytest -v

# ---------------------------------------------------------------------------
# REQ-NF-01 benchmark: cold 100MB Parquet scan over gdrive:// vs gs:// vs
# local. Gate is 3x GCS. Result is committed to docs/benchmark.md.
# ---------------------------------------------------------------------------
.PHONY: bench
bench: release
	@cd e2e && uv run python -m helpers.bench

# ---------------------------------------------------------------------------
# Static-linkage smoke test: the loadable extension may only dynamically link
# platform standard libraries.
# ---------------------------------------------------------------------------
.PHONY: smoke_static
smoke_static: release
	@./scripts/check_static_linkage.sh \
		build/release/extension/gdrive/gdrive.duckdb_extension

# ---------------------------------------------------------------------------
# Credential hygiene (REQ-NF-03). Fails if anything matching a Google key-file
# pattern is tracked by git, or if a token-shaped string appears in sources.
# .gitignore only protects people who never ran `git add -f`.
# ---------------------------------------------------------------------------
.PHONY: check_credentials
check_credentials:
	@./scripts/check_no_credentials.sh

# ---------------------------------------------------------------------------
# README verifier: extract every ```sql block from README.md and run it, so
# the docs cannot drift from the code.
# ---------------------------------------------------------------------------
.PHONY: verify_readme
verify_readme: release
	@python3 scripts/verify_readme.py

# ---------------------------------------------------------------------------
# The duckdb submodule must sit on the tag we ship against. A drifted
# submodule makes every local green meaningless -- see the script's header.
# ---------------------------------------------------------------------------
.PHONY: check_pin
check_pin:
	@./scripts/check_duckdb_pin.sh

# ---------------------------------------------------------------------------
# musl builds the 1.4 LTS matrix target linux_amd64_musl, whose headers do not
# pull in <cstdint> transitively. Missing includes are invisible on glibc.
# ---------------------------------------------------------------------------
.PHONY: check_cstdint
check_cstdint:
	@./scripts/check_cstdint.sh

# ---------------------------------------------------------------------------
# The SHIPPED artifact, not the statically linked shell. A static build can be
# green while the loadable extension refuses to load -- the DuckDB-version
# handshake and symbol resolution only happen at LOAD time.
# ---------------------------------------------------------------------------
.PHONY: check_stamp
check_stamp: release
	@./scripts/check_extension_stamp.sh

.PHONY: smoke_loadable
smoke_loadable: release
	@./scripts/smoke_loadable.sh

# ---------------------------------------------------------------------------
# Borrow DuckLake's own test suite and point its DATA PATH at gdrive://.
# Differential: only tests that pass locally and fail on Drive are findings.
# ---------------------------------------------------------------------------
.PHONY: ducklake_conformance
ducklake_conformance: release
	@python3 scripts/ducklake_conformance.py
