#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"
#include "storage/ducklake_transaction_changes.hpp"

namespace duckdb {

template<class T>
Value IDListToValue(const set<T> &id_list) {
	vector<Value> list_values;
	for (auto &id_entry : id_list) {
		list_values.emplace_back(to_string(id_entry.index));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list_values));
}

Value NameListToValue(const case_insensitive_set_t &list_val) {
	vector<Value> list_values;
	for (auto &entry_name : list_val) {
		list_values.emplace_back(entry_name);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list_values));
}

Value CatalogListToValue(const case_insensitive_map_t<case_insensitive_set_t> &list_val) {
	vector<Value> list_values;
	for (auto &entry : list_val) {
		auto schema = KeywordHelper::WriteOptionallyQuoted(entry.first);
		for(auto &entry_name : entry.second) {
			auto table = KeywordHelper::WriteOptionallyQuoted(entry_name);
			list_values.emplace_back(schema + "." + table);
		}
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list_values));
}

static unique_ptr<FunctionData> DuckLakeSnapshotsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &transaction = DuckLakeTransaction::Get(context, catalog);

	auto &metadata_manager = transaction.GetMetadataManager();
	auto snapshots = metadata_manager.GetAllSnapshots();
	auto result = make_uniq<MetadataBindData>();
	for (auto &snapshot : snapshots) {
		vector<Value> row_values;
		row_values.push_back(Value::BIGINT(snapshot.id));
		row_values.push_back(Value::TIMESTAMPTZ(snapshot.time));
		row_values.push_back(Value::BIGINT(snapshot.schema_version));

		auto other_changes = SnapshotChangeInformation::ParseChangesMade(snapshot.change_info.changes_made);
		vector<Value> change_keys;
		vector<Value> change_values;
		if (!other_changes.created_schemas.empty()) {
			change_keys.emplace_back("schemas_created");
			change_values.push_back(NameListToValue(other_changes.created_schemas));
		}
		if (!other_changes.dropped_schemas.empty()) {
			change_keys.emplace_back("schemas_dropped");
			change_values.push_back(IDListToValue(other_changes.dropped_schemas));
		}
		if (!other_changes.created_tables.empty()) {
			change_keys.emplace_back("tables_created");
			change_values.push_back(CatalogListToValue(other_changes.created_tables));
		}
		if (!other_changes.dropped_tables.empty()) {
			change_keys.emplace_back("tables_dropped");
			change_values.push_back(IDListToValue(other_changes.dropped_tables));
		}
		if (!other_changes.altered_tables.empty()) {
			change_keys.emplace_back("tables_altered");
			change_values.push_back(IDListToValue(other_changes.altered_tables));
		}
		if (!other_changes.inserted_tables.empty()) {
			change_keys.emplace_back("tables_inserted_into");
			change_values.push_back(IDListToValue(other_changes.inserted_tables));
		}
		if (!other_changes.tables_deleted_from.empty()) {
			change_keys.emplace_back("tables_deleted_from");
			change_values.push_back(IDListToValue(other_changes.tables_deleted_from));
		}
		if (!other_changes.dropped_views.empty()) {
			change_keys.emplace_back("views_dropped");
			change_values.push_back(IDListToValue(other_changes.dropped_views));
		}
		row_values.push_back(Value::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
		                                std::move(change_keys), std::move(change_values)));
		result->rows.push_back(std::move(row_values));
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

DuckLakeSnapshotsFunction::DuckLakeSnapshotsFunction()
    : BaseMetadataFunction("ducklake_snapshots", DuckLakeSnapshotsBind) {
}

} // namespace duckdb
