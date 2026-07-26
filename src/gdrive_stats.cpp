// S-4.1 (reporting surface) -- gdrive_stats() table function.
//
// Returns one row per DriveCallStats counter, reading the process-wide
// aggregate declared in gdrive_stats.hpp / defined in gdrive_client.cpp. Live
// tests assert on these numbers to prove the R-1 path-cache mitigation
// actually bounds Drive API amplification (docs/implementation-plan.md
// S-2.7 / S-2.9) -- if the counters lie, the mitigation is unmeasurable.
//
// Registration pattern follows ../quack-oauth/src/diagnose.cpp
// (quack_oauth_diagnose(): TableFunction with Bind/Init/Scan, no arguments,
// bind-time snapshot into TableFunctionData, cursor in GlobalTableFunctionState).
#include "gdrive_stats.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <utility>
#include <vector>

namespace duckdb {
namespace gdrive {

namespace {

struct StatRow {
	string metric;
	int64_t value;
};

struct GDriveStatsBindData : public TableFunctionData {
	vector<StatRow> rows;
};

struct GDriveStatsGlobalState : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

unique_ptr<FunctionData> GDriveStatsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                         vector<string> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
	names = {"metric", "value"};

	auto data = make_uniq<GDriveStatsBindData>();
	DriveCallStats stats = GetGlobalDriveCallStats();

	data->rows = {
	    {"files_get", stats.files_get},
	    {"files_list", stats.files_list},
	    {"files_media", stats.files_media},
	    {"files_export", stats.files_export},
	    {"files_create", stats.files_create},
	    {"files_update", stats.files_update},
	    {"files_delete", stats.files_delete},
	    {"cache_hits", stats.cache_hits},
	    {"cache_misses", stats.cache_misses},
	    {"retries", stats.retries},
	    {"total", stats.Total()},
	};
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> GDriveStatsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<GDriveStatsGlobalState>();
}

void GDriveStatsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<GDriveStatsBindData>();
	auto &state = input.global_state->Cast<GDriveStatsGlobalState>();
	idx_t out_row = 0;
	while (state.cursor < bind_data.rows.size() && out_row < STANDARD_VECTOR_SIZE) {
		const auto &row = bind_data.rows[state.cursor];
		output.SetValue(0, out_row, Value(row.metric));
		output.SetValue(1, out_row, Value::BIGINT(row.value));
		state.cursor++;
		out_row++;
	}
	output.SetCardinality(out_row);
}

} // namespace

void RegisterGDriveStats(ExtensionLoader &loader) {
	TableFunction fn("gdrive_stats", {}, GDriveStatsScan, GDriveStatsBind, GDriveStatsInit);
	CreateTableFunctionInfo info(fn);

	FunctionDescription desc;
	desc.description =
	    "Drive API call counters, one row per metric: files_get, files_list, files_media, files_export, "
	    "files_create, files_update, files_delete (calls by kind), cache_hits/cache_misses (the path-resolution "
	    "cache that mitigates R-1 amplification), retries (retried HTTP attempts across all kinds), and total "
	    "(sum of the files_* kind counters). Process-wide, not reset between queries -- call it before and "
	    "after an operation and diff the two snapshots to measure that operation's amplification.";
	desc.parameter_names = {};
	desc.parameter_types = {};
	desc.examples = {"SELECT * FROM gdrive_stats()"};
	desc.categories = {"gdrive"};
	info.descriptions.push_back(std::move(desc));

	loader.RegisterFunction(std::move(info));
}

} // namespace gdrive
} // namespace duckdb
