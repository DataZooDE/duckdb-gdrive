// S-0.2 -- proves the Catch2 pure-logic binary exists, builds without DuckDB
// linkage, and runs. Deliberately trivial; its job is the wiring, not the
// assertion.
#include <catch2/catch_test_macros.hpp>

#include "gdrive_version.hpp"

using duckdb::gdrive::GdriveVersion;

TEST_CASE("version is a non-empty semver-ish string", "[version]") {
	const auto v = GdriveVersion();
	REQUIRE_FALSE(v.empty());
	REQUIRE(v.find('.') != std::string::npos);
}
