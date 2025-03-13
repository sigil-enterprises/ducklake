#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "common/ducklake_util.hpp"

namespace duckdb {

struct DuckLakeSnapshotsBindData : public TableFunctionData {
	DuckLakeSnapshotsBindData() {
	}

	vector<vector<Value>> snapshots;
};

static unique_ptr<FunctionData> DuckLakeSnapshotsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("Parameters to ducklake_snaphots cannot be NULL");
	}
	// look up the database to query
	auto db_name = input.inputs[0].GetValue<string>();
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, db_name);
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" referenced in ducklake_snapshots()", db_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "ducklake") {
		throw BinderException("Attached database \"%s\" does not refer to a DuckLake database", db_name);
	}
	auto &transaction = DuckLakeTransaction::Get(context, catalog);

	vector<string> change_sets {"schemas_created", "schemas_dropped",      "tables_created",     "tables_dropped",
	                            "tables_altered",  "tables_inserted_into", "tables_deleted_from"};
	auto res = transaction.Query(R"(
SELECT snapshot_id, snapshot_time, schema_version, schemas_created, schemas_dropped, tables_created, tables_dropped, tables_altered, tables_inserted_into, tables_deleted_from
FROM {METADATA_CATALOG}.ducklake_snapshot
LEFT JOIN {METADATA_CATALOG}.ducklake_snapshot_changes USING (snapshot_id)
ORDER BY snapshot_id
)");
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to get snapshot information from DuckLake: ");
	}
	auto result = make_uniq<DuckLakeSnapshotsBindData>();
	for (auto &row : *res) {
		vector<Value> row_values;
		auto snapshot_id = row.GetValue<idx_t>(0);
		auto snapshot_time = row.GetValue<timestamp_tz_t>(1);
		auto schema_version = row.GetValue<idx_t>(2);

		vector<Value> change_keys;
		vector<Value> change_values;

		for (idx_t c = 0; c < change_sets.size(); c++) {
			idx_t col_idx = c + 3;
			if (row.IsNull(col_idx)) {
				continue;
			}
			auto changes = row.GetValue<string>(col_idx);
			auto &change_name = change_sets[c];
			vector<Value> change_list_values;
			if (change_name == "schemas_dropped" || change_name == "tables_dropped" ||
			    change_name == "tables_inserted_into") {
				auto drop_list = DuckLakeUtil::ParseDropList(changes);
				for (auto &drop_entry : drop_list) {
					change_list_values.emplace_back(to_string(drop_entry));
				}
			} else if (change_name == "tables_created") {
				auto created_list = DuckLakeUtil::ParseTableList(changes);
				for (auto &entry : created_list) {
					auto schema = KeywordHelper::WriteOptionallyQuoted(entry.schema);
					auto table = KeywordHelper::WriteOptionallyQuoted(entry.table);
					change_list_values.emplace_back(schema + "." + table);
				}
			} else {
				auto change_list = DuckLakeUtil::ParseQuotedList(changes);
				for (auto &change_entry : change_list) {
					change_list_values.emplace_back(change_entry);
				}
			}
			change_keys.push_back(Value(change_name));
			change_values.push_back(Value::LIST(LogicalType::VARCHAR, std::move(change_list_values)));
		}
		row_values.push_back(Value::BIGINT(snapshot_id));
		row_values.push_back(Value::TIMESTAMPTZ(snapshot_time));
		row_values.push_back(Value::BIGINT(schema_version));
		row_values.push_back(Value::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
		                                std::move(change_keys), std::move(change_values)));
		result->snapshots.push_back(std::move(row_values));
	}

	names.emplace_back("snapshot_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("snapshot_time");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);

	names.emplace_back("schema_version");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("changes");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)));

	return std::move(result);
}

struct DuckLakeSnapshotsData : public GlobalTableFunctionState {
	DuckLakeSnapshotsData() : offset(0) {
	}

	idx_t offset;
};

unique_ptr<GlobalTableFunctionState> DuckLakeSnapshotsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckLakeSnapshotsData>();
	return std::move(result);
}

void DuckLakeSnapshotsExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<DuckLakeSnapshotsBindData>();
	auto &state = data_p.global_state->Cast<DuckLakeSnapshotsData>();
	if (state.offset >= data.snapshots.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;
	while (state.offset < data.snapshots.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.snapshots[state.offset++];

		for (idx_t c = 0; c < entry.size(); c++) {
			output.SetValue(c, count, entry[c]);
		}
		count++;
	}
	output.SetCardinality(count);
}

DuckLakeSnapshotsFunction::DuckLakeSnapshotsFunction()
    : TableFunction("ducklake_snapshots", {LogicalType::VARCHAR}, DuckLakeSnapshotsExecute, DuckLakeSnapshotsBind,
                    DuckLakeSnapshotsInit) {
}

} // namespace duckdb
