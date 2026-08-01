// S-0.2 -- proves the Catch2 pure-logic binary exists, builds without DuckDB
// linkage, and runs. Deliberately trivial; its job is the wiring, not the
// assertion.
#include <catch2/catch_test_macros.hpp>

#include "gdrive_version.hpp"

using duckdb::gdrive::GdriveVersion;

TEST_CASE("version is a non-empty dotted string", "[version]") {
	// Shape only, deliberately format-agnostic: the project moved from semver
	// to CalVer (vYYYY.MM.DD) and a test that pinned the old shape would have
	// failed for a reason that has nothing to do with correctness. The tag and
	// this string are kept in step by check_extension_stamp.sh instead.
	const auto v = GdriveVersion();
	REQUIRE_FALSE(v.empty());
	REQUIRE(v.find('.') != std::string::npos);
}
