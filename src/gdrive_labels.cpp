// Drive labels -> rows. PURE: see gdrive_labels.hpp.
#include "gdrive_labels.hpp"

// jwt-cpp's picojson traits header, not picojson directly: the
// PICOJSON_USE_INT64 ODR trap described in CMakeLists.txt.
#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

#include <algorithm>
#include <cstdint>

namespace duckdb {
namespace gdrive {

namespace {

const picojson::object *Obj(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<picojson::object>()) ? &it->second.get<picojson::object>() : nullptr;
}

const picojson::array *Arr(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<picojson::array>()) ? &it->second.get<picojson::array>() : nullptr;
}

std::string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<std::string>()) ? it->second.get<std::string>() : std::string();
}

bool ParseTop(const std::string &json, picojson::value &holder, std::string &error) {
	const auto err = picojson::parse(holder, json);
	if (!err.empty()) {
		error = "malformed JSON: " + err;
		return false;
	}
	if (!holder.is<picojson::object>()) {
		error = "expected a JSON object";
		return false;
	}
	return true;
}

//! One value of an applied field as text. Google sends int64 as a decimal
//! string; a bare number is accepted too.
std::string ValueText(const picojson::value &v) {
	if (v.is<std::string>()) {
		return v.get<std::string>();
	}
	if (v.is<int64_t>()) {
		return std::to_string(v.get<int64_t>());
	}
	if (v.is<picojson::object>()) {
		// A user: the address, else the display name Drive shows instead.
		const auto &u = v.get<picojson::object>();
		auto email = Str(u, "emailAddress");
		return email.empty() ? Str(u, "displayName") : email;
	}
	return std::string();
}

//! The display name under `properties`, as the Labels API nests it.
std::string DisplayName(const picojson::object &o, const char *key) {
	auto props = Obj(o, "properties");
	return props ? Str(*props, key) : std::string();
}

} // namespace

bool ParseAppliedLabels(const std::string &json_body, std::vector<AppliedLabel> &out, std::string &next_page_token,
                        std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	const auto &top = holder.get<picojson::object>();
	next_page_token = Str(top, "nextPageToken");
	auto labels = Arr(top, "labels");
	if (!labels) {
		// Drive sends the empty array for a file with no labels (captured
		// 2026-11-13); its absence means this is not a listLabels answer.
		error = "the response has no 'labels' array";
		return false;
	}
	for (auto &lv : *labels) {
		if (!lv.is<picojson::object>()) {
			continue;
		}
		const auto &l = lv.get<picojson::object>();
		AppliedLabel label;
		label.id = Str(l, "id");
		if (label.id.empty()) {
			error = "a label without an id";
			return false;
		}
		label.revision_id = Str(l, "revisionId");
		if (auto fields = Obj(l, "fields")) {
			for (auto &kv : *fields) {
				if (!kv.second.is<picojson::object>()) {
					continue;
				}
				const auto &f = kv.second.get<picojson::object>();
				AppliedLabelField field;
				field.id = Str(f, "id");
				if (field.id.empty()) {
					field.id = kv.first;
				}
				field.value_type = Str(f, "valueType");
				// The values sit in the array named after the type; reading it
				// by name keeps an unknown future type from being misread.
				if (!field.value_type.empty()) {
					if (auto values = Arr(f, field.value_type.c_str())) {
						for (auto &v : *values) {
							auto text = ValueText(v);
							if (!text.empty()) {
								field.values.push_back(std::move(text));
							}
						}
					}
				}
				label.fields.push_back(std::move(field));
			}
			std::sort(label.fields.begin(), label.fields.end(),
			          [](const AppliedLabelField &a, const AppliedLabelField &b) { return a.id < b.id; });
		}
		out.push_back(std::move(label));
	}
	return true;
}

std::string LabelCatalogFields() {
	return "nextPageToken,labels(id,properties(title),fields(id,properties(displayName),"
	       "selectionOptions(choices(id,properties(displayName)))))";
}

bool ParseLabelCatalog(const std::string &json_body, LabelCatalog &catalog, std::string &next_page_token,
                       std::string &error) {
	picojson::value holder;
	if (!ParseTop(json_body, holder, error)) {
		return false;
	}
	const auto &top = holder.get<picojson::object>();
	if (top.count("error")) {
		error = "the response is an error, not a label catalog";
		return false;
	}
	next_page_token = Str(top, "nextPageToken");
	auto labels = Arr(top, "labels");
	if (!labels) {
		return true;
	}
	for (auto &lv : *labels) {
		if (!lv.is<picojson::object>()) {
			continue;
		}
		const auto &l = lv.get<picojson::object>();
		const auto id = Str(l, "id");
		if (id.empty()) {
			continue;
		}
		auto &names = catalog[id];
		names.title = DisplayName(l, "title");
		if (auto fields = Arr(l, "fields")) {
			for (auto &fv : *fields) {
				if (!fv.is<picojson::object>()) {
					continue;
				}
				const auto &f = fv.get<picojson::object>();
				const auto field_id = Str(f, "id");
				if (field_id.empty()) {
					continue;
				}
				names.fields[field_id] = DisplayName(f, "displayName");
				auto sel = Obj(f, "selectionOptions");
				auto choices = sel ? Arr(*sel, "choices") : nullptr;
				if (choices) {
					auto &map = names.choices[field_id];
					for (auto &cv : *choices) {
						if (!cv.is<picojson::object>()) {
							continue;
						}
						const auto &c = cv.get<picojson::object>();
						const auto choice_id = Str(c, "id");
						if (!choice_id.empty()) {
							map[choice_id] = DisplayName(c, "displayName");
						}
					}
				}
			}
		}
	}
	return true;
}

std::vector<LabelRow> LabelRows(const std::vector<AppliedLabel> &labels, const LabelCatalog *catalog) {
	std::vector<LabelRow> rows;
	for (auto &label : labels) {
		const LabelNames *names = nullptr;
		if (catalog) {
			auto it = catalog->find(label.id);
			if (it != catalog->end()) {
				names = &it->second;
			}
		}
		LabelRow base;
		base.label_id = label.id;
		base.label = names ? names->title : std::string();
		bool any = false;
		for (auto &field : label.fields) {
			LabelRow row = base;
			row.has_field = true;
			row.field_id = field.id;
			row.value_type = field.value_type;
			const std::map<std::string, std::string> *choices = nullptr;
			if (names) {
				auto f = names->fields.find(field.id);
				if (f != names->fields.end()) {
					row.field = f->second;
				}
				auto c = names->choices.find(field.id);
				if (c != names->choices.end()) {
					choices = &c->second;
				}
			}
			if (field.values.empty()) {
				// A field Drive lists with no value: still a row, value empty.
				rows.push_back(row);
				any = true;
				continue;
			}
			for (auto &value : field.values) {
				LabelRow r = row;
				r.value = value;
				if (choices && field.value_type == "selection") {
					auto name = choices->find(value);
					if (name != choices->end() && !name->second.empty()) {
						r.value = name->second;
					}
				}
				rows.push_back(std::move(r));
				any = true;
			}
		}
		if (!any) {
			rows.push_back(base);
		}
	}
	return rows;
}

} // namespace gdrive
} // namespace duckdb
