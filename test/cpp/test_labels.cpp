// Drive labels -> gdrive_labels rows. Pure logic, no DuckDB linkage.
//
// ONLY captured bodies here (D-1: data, not mocks). As of 2026-11-13 the CI
// Workspace has no label published and the Labels API is not enabled in the
// CI project, so the only real bodies there are are an EMPTY listLabels
// answer (captured as the CI service account and as the delegated user, on a
// /fixtures file) and the error bodies in test_errors.cpp. The parsing of an
// applied label with field values, and of a catalog, is therefore NOT pinned
// here yet: capture those bodies from the label fixture described in
// scripts/setup_ci_drive.sh, redacted per test_rows.cpp's header, and add
// them. Writing them by hand would test the parser against our reading of the
// docs, not against Drive.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "gdrive_labels.hpp"

using namespace duckdb::gdrive;

namespace {

const char *kListLabelsEmpty = R"({
  "labels": [],
  "kind": "drive#labelList"
})";

} // namespace

TEST_CASE("ParseAppliedLabels: a file with no labels", "[labels]") {
	std::vector<AppliedLabel> labels;
	std::string next, error;
	REQUIRE(ParseAppliedLabels(kListLabelsEmpty, labels, next, error));
	CHECK(labels.empty());
	CHECK(next.empty());
	CHECK(LabelRows(labels, nullptr).empty());
}

TEST_CASE("ParseAppliedLabels: an error body is not an empty label list", "[labels]") {
	std::vector<AppliedLabel> labels;
	std::string next, error;
	CHECK_FALSE(ParseAppliedLabels(R"({"error": {"code": 403}})", labels, next, error));
	CHECK(error.find("'labels'") != std::string::npos);
	CHECK_FALSE(ParseAppliedLabels("not json", labels, next, error));
}

TEST_CASE("ParseLabelCatalog: an error body is not an empty catalog", "[labels]") {
	LabelCatalog catalog;
	std::string next, error;
	CHECK_FALSE(ParseLabelCatalog(R"({"error": {"code": 403, "status": "PERMISSION_DENIED"}})", catalog, next, error));
	CHECK(catalog.empty());
}
