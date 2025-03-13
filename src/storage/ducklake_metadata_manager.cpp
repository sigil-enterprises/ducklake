#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

DuckLakeMetadataManager::DuckLakeMetadataManager(DuckLakeTransaction &transaction) : transaction(transaction) {
}

DuckLakeMetadataManager::~DuckLakeMetadataManager() {
}

DuckLakeMetadataManager &DuckLakeMetadataManager::Get(DuckLakeTransaction &transaction) {
	return transaction.GetMetadataManager();
}

void DuckLakeMetadataManager::DropSchemas(DuckLakeSnapshot commit_snapshot, unordered_set<idx_t> ids) {
	FlushDrop(commit_snapshot, "ducklake_schema", "schema_id", ids);
}

void DuckLakeMetadataManager::DropTables(DuckLakeSnapshot commit_snapshot, unordered_set<idx_t> ids) {
	FlushDrop(commit_snapshot, "ducklake_table", "table_id", ids);
}

void DuckLakeMetadataManager::FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name,
                                        const string &id_name, unordered_set<idx_t> &dropped_entries) {
	string dropped_id_list;
	for (auto &dropped_id : dropped_entries) {
		if (!dropped_id_list.empty()) {
			dropped_id_list += ", ";
		}
		dropped_id_list += to_string(dropped_id);
	}
	auto dropped_id_query =
	    StringUtil::Format(R"(UPDATE {METADATA_CATALOG}.%s SET end_snapshot = {SNAPSHOT_ID} WHERE %s IN (%s);)",
	                       metadata_table_name, id_name, dropped_id_list);
	auto result = transaction.Query(commit_snapshot, dropped_id_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write dropped table information to DuckLake:");
	}
}

void DuckLakeMetadataManager::WriteNewSchemas(DuckLakeSnapshot commit_snapshot,
                                              const vector<DuckLakeSchemaInfo> &new_schemas) {
	if (new_schemas.empty()) {
		throw InternalException("No schemas to create - should be handled elsewhere");
	}
	string schema_insert_sql;
	for (auto &new_schema : new_schemas) {
		if (!schema_insert_sql.empty()) {
			schema_insert_sql += ",";
		}
		schema_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %s)", new_schema.id, new_schema.uuid,
		                                        SQLString(new_schema.name));
	}
	schema_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_schema VALUES " + schema_insert_sql;
	auto result = transaction.Query(commit_snapshot, schema_insert_sql);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new schemas to DuckLake: ");
	}
}

} // namespace duckdb
