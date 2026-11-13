#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace duckdb {
namespace gdrive {

// ---------------------------------------------------------------------------
// Drive labels -> rows, for gdrive_labels(). PURE: no duckdb.hpp, no I/O,
// never throws; parsers return false with `error` set.
//
// Two APIs are involved, and only the first is needed:
//  - Drive v3 files.listLabels: the labels APPLIED to one file, by id, with
//    their field values. Works with the Drive scope alone.
//  - Drive Labels API v2 labels.list (drivelabels.googleapis.com): the label
//    CATALOG, which is where the human-readable names live -- the label's
//    title, each field's display name, each selection choice's display name.
//    Needs the drive.labels(.readonly) scope AND the API enabled in the
//    credential's Google Cloud project, hence opt-in (names := true).
// ---------------------------------------------------------------------------

//! One field of an applied label. Drive sends each value kind in its own
//! array (dateString, integer, selection, text, user); `values` holds
//! whichever was present, as text: a user as their address (display name if
//! Drive hides it), a selection as its choice id, an integer in decimal, a
//! date as YYYY-MM-DD.
struct AppliedLabelField {
	std::string id;
	std::string value_type;
	std::vector<std::string> values;
};

//! One label applied to a file (files.listLabels).
struct AppliedLabel {
	std::string id;
	std::string revision_id;
	//! Sorted by field id, so rows come out in a stable order.
	std::vector<AppliedLabelField> fields;
};

//! A files.listLabels page. `next_page_token` is empty on the last page.
bool ParseAppliedLabels(const std::string &json_body, std::vector<AppliedLabel> &out, std::string &next_page_token,
                        std::string &error);

//! What the catalog knows about one label: names, by id.
struct LabelNames {
	std::string title;
	//! field id -> display name.
	std::map<std::string, std::string> fields;
	//! field id -> (choice id -> display name), selection fields only.
	std::map<std::string, std::map<std::string, std::string>> choices;
};

//! label id -> its names.
using LabelCatalog = std::map<std::string, LabelNames>;

//! The `fields` mask for labels.list: only what LabelRows reads.
std::string LabelCatalogFields();

//! One labels.list page, ADDED to `catalog`. An empty catalog is a valid
//! answer, and Google omits the empty array, so `{}` parses as no labels.
bool ParseLabelCatalog(const std::string &json_body, LabelCatalog &catalog, std::string &next_page_token,
                       std::string &error);

//! One row of gdrive_labels.
struct LabelRow {
	std::string label_id;
	//! The label's title; empty without a catalog or when it lacks the label.
	std::string label;
	//! Empty, with has_field false, for a label applied with no field set.
	std::string field_id;
	std::string field;
	std::string value_type;
	std::string value;
	bool has_field = false;
};

//! Rows for one file: one per (label, field, value); a label with no field
//! set is one row with no field. With a catalog, names are filled in and a
//! selection value is its choice's display name instead of its id.
std::vector<LabelRow> LabelRows(const std::vector<AppliedLabel> &labels, const LabelCatalog *catalog);

} // namespace gdrive
} // namespace duckdb
