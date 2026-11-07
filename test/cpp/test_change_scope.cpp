// Whether a changed file is under the folder gdrive_changes watches, and at
// what path. Pure logic, no DuckDB linkage: the folder lookups are a callback,
// so the tree below is plain data.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "gdrive_change_scope.hpp"

using duckdb::gdrive::ChangeScope;
using duckdb::gdrive::ChangeScopeNode;

namespace {

//! root
//!  +- F (the watched folder)
//!  |   +- a
//!  |   |   +- b
//!  |   +- c
//!  +- elsewhere
std::map<std::string, ChangeScopeNode> Tree() {
	return {{"root", {"My Drive", {}}},  {"F", {"Finance", {"root"}}}, {"a", {"2026", {"F"}}},
	        {"b", {"q4", {"a"}}},         {"c", {"archive", {"F"}}},  {"elsewhere", {"Other", {"root"}}}};
}

ChangeScope ScopeOver(const std::map<std::string, ChangeScopeNode> &tree, const std::string &folder) {
	return ChangeScope(folder, [tree](const std::string &id, ChangeScopeNode &node) {
		auto it = tree.find(id);
		if (it == tree.end()) {
			return false;
		}
		node = it->second;
		return true;
	});
}

} // namespace

TEST_CASE("ChangeScope: files under the folder get their path below it", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel;
	REQUIRE(scope.Locate("top.csv", {"F"}, rel));
	CHECK(rel == "top.csv");
	REQUIRE(scope.Locate("deep.csv", {"b"}, rel));
	CHECK(rel == "2026/q4/deep.csv");
	REQUIRE(scope.Locate("old.csv", {"c"}, rel));
	CHECK(rel == "archive/old.csv");
}

TEST_CASE("ChangeScope: outside the folder, or above it, is out of scope", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel = "unchanged";
	CHECK_FALSE(scope.Locate("x.csv", {"elsewhere"}, rel));
	CHECK_FALSE(scope.Locate("x.csv", {"root"}, rel));
	CHECK_FALSE(scope.Locate("orphan.csv", {}, rel));
	// A parent this identity cannot see: out, not an error.
	CHECK_FALSE(scope.Locate("x.csv", {"unshared"}, rel));
	CHECK(rel == "unchanged");
}

TEST_CASE("ChangeScope: each folder is looked up once", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel;
	REQUIRE(scope.Locate("1.csv", {"b"}, rel));
	const auto first = scope.Lookups();
	CHECK(first == 2); // b, then a; F is known
	for (int i = 0; i < 50; i++) {
		REQUIRE(scope.Locate("n.csv", {"b"}, rel));
		REQUIRE(scope.Locate("n.csv", {"a"}, rel));
	}
	CHECK(scope.Lookups() == first);
	CHECK_FALSE(scope.Locate("x.csv", {"elsewhere"}, rel));
	CHECK(scope.Lookups() == first + 2); // elsewhere, root
	CHECK_FALSE(scope.Locate("y.csv", {"elsewhere"}, rel));
	CHECK(scope.Lookups() == first + 2);
}

TEST_CASE("ChangeScope: any parent that leads to the folder will do", "[changes][scope]") {
	auto scope = ScopeOver(Tree(), "F");
	std::string rel;
	REQUIRE(scope.Locate("shared.csv", {"elsewhere", "c"}, rel));
	CHECK(rel == "archive/shared.csv");
}

TEST_CASE("ChangeScope: a parent cycle ends instead of recursing", "[changes][scope]") {
	std::map<std::string, ChangeScopeNode> tree {{"x", {"x", {"y"}}}, {"y", {"y", {"x"}}}};
	auto scope = ScopeOver(tree, "F");
	std::string rel;
	CHECK_FALSE(scope.Locate("f.csv", {"x"}, rel));
	CHECK(scope.Lookups() == 2);
}

TEST_CASE("ChangeScope: a failing lookup propagates", "[changes][scope]") {
	ChangeScope scope("F", [](const std::string &, ChangeScopeNode &) -> bool { throw std::runtime_error("403"); });
	std::string rel;
	CHECK_THROWS_AS(scope.Locate("f.csv", {"a"}, rel), std::runtime_error);
}
