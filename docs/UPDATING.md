# Updating to a new DuckDB release

DuckDB extensions are ABI-tied to the version they were built against, so this
repository must be rebuilt for every DuckDB release. REQ-NF-06 records this as
a standing cost for the life of the project, not a one-off — it is written
down here so it is a routine rather than a rediscovery.

Current targets: **stable v1.5.5** and the **1.4 LTS** line (source v1.4.5).
Local development builds against stable only; CI builds both.

## The routine

1. **Bump the submodules.**

   ```bash
   git -C duckdb fetch --tags && git -C duckdb checkout v<new>
   git -C extension-ci-tools fetch && git -C extension-ci-tools checkout v<new>
   ```

   Note the asymmetry: `duckdb` uses **tags**, `extension-ci-tools` uses
   version **branches**. `git submodule add --depth 1 -b <tag>` fails on the
   former, because `-b` wants a branch.

2. **Clean and rebuild.** A DuckDB bump is one of the few times `make clean` is
   genuinely required — stale objects compiled against the old headers link
   without complaint and then misbehave at runtime.

   ```bash
   make clean && GEN=ninja make
   ```

3. **Check the `FileSystem` interface first.** This extension subclasses
   `duckdb::FileSystem`, which is not a stable public API and does change
   between releases. Diff it:

   ```bash
   git -C duckdb diff v<old>..v<new> -- src/include/duckdb/common/file_system.hpp
   ```

   Watch specifically for: methods gaining or losing `virtual`; signatures
   moving between `const string &` and `const OpenFileInfo &`; new
   `*Extended` hooks with `Supports*Extended()` opt-ins. **Overriding a method
   that is no longer virtual compiles cleanly and is then simply never
   called** — a silent behavioural regression that no compiler warns about.
   The v1.5.5 `OpenFile` overload set has exactly this shape.

4. **Run every layer.**

   ```bash
   make test          # pure-logic + SQL
   make test_live     # against real Drive; needs credentials
   make e2e
   make smoke_static  # dynamic-dependency allowlist
   ```

   The live suite is the one that matters here: the pure layers cannot detect
   an interface drift, because they do not link DuckDB at all.

5. **Update the CI matrix** in `.github/workflows/MainDistributionPipeline.yml`
   — `duckdb_version`, `ci_tools_version`, and the workflow `@ref`.

6. **Re-check the two build workarounds** in `extension_config.cmake` (the
   project-wide C++17 `FORCE`, and the `_HAS_STD_BYTE=0` guard scoped to
   1.4.x). Both exist for specific upstream problems and may become
   unnecessary — or newly necessary elsewhere. Do not delete either without
   confirming a clean build on **Linux and Windows**; each one covers a
   failure that appears on only one platform.

7. **Tag and release.** The distribution pipeline publishes per-version paths,
   so stable and LTS never collide.

## Things that have bitten before

- The build requires **at least one git commit**: DuckDB's version stamping
  runs `git log -1 --format=%h` and fails configure with a confusing error on
  a fresh repository.
- `PICOJSON_USE_INT64` must stay defined project-wide. If a new dependency
  pulls in picojson without it, numbers silently become `double` in that
  translation unit only — an ODR violation whose symptom depends on link
  order.
- Windows builds must use MSVC `cl`. Letting the runner autodetect picks MinGW
  g++ on current images, which trips a `std::byte` ambiguity against the
  Win-SDK and cannot link the MSVC-built vcpkg OpenSSL.
