#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "common/ducklake_util.hpp"

namespace duckdb {

Value IDListToValue(const string &list_val) {
	vector<Value> list_values;
	auto drop_list = DuckLakeUtil::ParseDropList(list_val);
	for (auto &drop_entry : drop_list) {
		list_values.emplace_back(to_string(drop_entry));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list_values));
}

Value QuotedListToValue(const string &list_val) {
	vector<Value> list_values;
	auto change_list = DuckLakeUtil::ParseQuotedList(list_val);
	for (auto &change_entry : change_list) {
		list_values.emplace_back(change_entry);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list_values));
}

Value TableListToValue(const string &list_val) {
	vector<Value> list_values;
	auto created_list = DuckLakeUtil::ParseTableList(list_val);
	for (auto &entry : created_list) {
		auto schema = KeywordHelper::WriteOptionallyQuoted(entry.schema);
		auto table = KeywordHelper::WriteOptionallyQuoted(entry.table);
		list_values.emplace_back(schema + "." + table);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list_values));
}

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

	auto &metadata_manager = transaction.GetMetadataManager();
	auto snapshots = metadata_manager.GetAllSnapshots();
	auto result = make_uniq<MetadataBindData>();
	for (auto &snapshot : snapshots) {
		vector<Value> row_values;
		row_values.push_back(Value::BIGINT(snapshot.id));
		row_values.push_back(Value::TIMESTAMPTZ(snapshot.time));
		row_values.push_back(Value::BIGINT(snapshot.schema_version));

		vector<Value> change_keys;
		vector<Value> change_values;
		if (!snapshot.change_info.schemas_created.empty()) {
			change_keys.emplace_back("schemas_created");
			change_values.push_back(QuotedListToValue(snapshot.change_info.schemas_created));
		}
		if (!snapshot.change_info.schemas_dropped.empty()) {
			change_keys.emplace_back("schemas_dropped");
			change_values.push_back(IDListToValue(snapshot.change_info.schemas_dropped));
		}
		if (!snapshot.change_info.tables_created.empty()) {
			change_keys.emplace_back("tables_created");
			change_values.push_back(TableListToValue(snapshot.change_info.tables_created));
		}
		if (!snapshot.change_info.tables_dropped.empty()) {
			change_keys.emplace_back("tables_dropped");
			change_values.push_back(IDListToValue(snapshot.change_info.tables_dropped));
		}
		if (!snapshot.change_info.tables_altered.empty()) {
			change_keys.emplace_back("tables_altered");
			change_values.push_back(IDListToValue(snapshot.change_info.tables_altered));
		}
		if (!snapshot.change_info.tables_inserted_into.empty()) {
			change_keys.emplace_back("tables_inserted_into");
			change_values.push_back(IDListToValue(snapshot.change_info.tables_inserted_into));
		}
		if (!snapshot.change_info.tables_deleted_from.empty()) {
			change_keys.emplace_back("tables_deleted_from");
			change_values.push_back(IDListToValue(snapshot.change_info.tables_deleted_from));
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
