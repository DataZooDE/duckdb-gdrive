// Catch2: the two pure halves of duplicate-name prevention and healing
// (gdrive_duplicates.*). Pure: no duckdb.hpp, no I/O. The Drive side -- a
// write right after our own create, and a real pair of identical copies -- is
// proven live (e2e/tests/test_duplicate_names.py).
#include "gdrive_duplicates.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace duckdb::gdrive;

TEST_CASE("a create is recalled within the window and forgotten after it", "[duplicates]") {
	RecentCreates recent(/*max_entries=*/8, /*window_ms=*/1000);
	recent.Remember("k", "id-1", /*now_ms=*/0);
	REQUIRE(recent.Recall("k", 999) == "id-1");
	REQUIRE(recent.Recall("k", 1000).empty());
	// An expired entry is dropped, not kept around.
	REQUIRE(recent.Size() == 0);
}

TEST_CASE("an unknown key recalls nothing", "[duplicates]") {
	RecentCreates recent(8, 1000);
	REQUIRE(recent.Recall("nope", 0).empty());
}

TEST_CASE("the newest create for a key wins, and Forget drops it", "[duplicates]") {
	RecentCreates recent(8, 1000);
	recent.Remember("k", "id-1", 0);
	recent.Remember("k", "id-2", 10);
	REQUIRE(recent.Recall("k", 20) == "id-2");
	recent.Forget("k");
	REQUIRE(recent.Recall("k", 20).empty());
}

TEST_CASE("the registry is bounded: the oldest entry goes first", "[duplicates]") {
	RecentCreates recent(/*max_entries=*/2, 100000);
	recent.Remember("a", "1", 0);
	recent.Remember("b", "2", 1);
	recent.Remember("c", "3", 2);
	REQUIRE(recent.Size() == 2);
	REQUIRE(recent.Recall("a", 3).empty());
	REQUIRE(recent.Recall("b", 3) == "2");
	REQUIRE(recent.Recall("c", 3) == "3");
}

namespace {
DuplicateCopy Copy(const std::string &id, const std::string &md5, int64_t size, const std::string &created,
                   const std::string &mime = "text/markdown") {
	DuplicateCopy c;
	c.id = id;
	c.md5 = md5;
	c.size = size;
	c.created_time = created;
	c.mime_type = mime;
	return c;
}
} // namespace

TEST_CASE("identical copies heal: the oldest is kept, the rest are trashed", "[duplicates]") {
	// The heron incident (2026-11-18): two agent.md, 3 s apart, same bytes.
	auto plan = PlanIdenticalDuplicateHeal({
	    Copy("1zHV", "abc", 3727, "2026-11-18T06:55:57.203Z"),
	    Copy("1Tob", "abc", 3727, "2026-11-18T06:55:54.427Z"),
	});
	REQUIRE(plan.healable);
	REQUIRE(plan.keep_id == "1Tob");
	REQUIRE(plan.trash_ids == std::vector<std::string> {"1zHV"});
}

TEST_CASE("a creation-time tie is broken by id, so every caller picks the same survivor", "[duplicates]") {
	auto plan = PlanIdenticalDuplicateHeal({
	    Copy("b", "abc", 1, "2026-01-01T00:00:00.000Z"),
	    Copy("a", "abc", 1, "2026-01-01T00:00:00.000Z"),
	    Copy("c", "abc", 1, "2026-01-01T00:00:00.000Z"),
	});
	REQUIRE(plan.healable);
	REQUIRE(plan.keep_id == "a");
	REQUIRE(plan.trash_ids == std::vector<std::string> {"b", "c"});
}

TEST_CASE("copies whose bytes differ are never healed", "[duplicates]") {
	auto plan = PlanIdenticalDuplicateHeal({
	    Copy("a", "abc", 3, "2026-01-01T00:00:00.000Z"),
	    Copy("b", "xyz", 3, "2026-01-01T00:00:01.000Z"),
	});
	REQUIRE_FALSE(plan.healable);
	REQUIRE(plan.keep_id.empty());
	REQUIRE(plan.trash_ids.empty());
	REQUIRE_FALSE(plan.reason.empty());
}

TEST_CASE("the same checksum with a different size is not identical", "[duplicates]") {
	auto plan = PlanIdenticalDuplicateHeal({
	    Copy("a", "abc", 3, "2026-01-01T00:00:00.000Z"),
	    Copy("b", "abc", 4, "2026-01-01T00:00:01.000Z"),
	});
	REQUIRE_FALSE(plan.healable);
}

TEST_CASE("a copy without a checksum cannot be proven identical", "[duplicates]") {
	// Native Google files and folders have no md5Checksum: there is nothing to
	// compare, so nothing is trashed.
	REQUIRE_FALSE(PlanIdenticalDuplicateHeal({
	                  Copy("a", "", 3, "2026-01-01T00:00:00.000Z"),
	                  Copy("b", "", 3, "2026-01-01T00:00:01.000Z"),
	              })
	                  .healable);
	REQUIRE_FALSE(PlanIdenticalDuplicateHeal({
	                  Copy("a", "abc", 3, "2026-01-01T00:00:00.000Z", "application/vnd.google-apps.folder"),
	                  Copy("b", "abc", 3, "2026-01-01T00:00:01.000Z", "application/vnd.google-apps.folder"),
	              })
	                  .healable);
}

TEST_CASE("a single match is not a duplicate", "[duplicates]") {
	REQUIRE_FALSE(PlanIdenticalDuplicateHeal({Copy("a", "abc", 3, "2026-01-01T00:00:00.000Z")}).healable);
	REQUIRE_FALSE(PlanIdenticalDuplicateHeal({}).healable);
}
