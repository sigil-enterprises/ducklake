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

void DuckLakeMetadataManager::WriteNewTables(DuckLakeSnapshot commit_snapshot,
                                             const vector<DuckLakeTableInfo> &new_tables) {
	string column_insert_sql;
	string table_insert_sql;
	for (auto &table : new_tables) {
		if (!table_insert_sql.empty()) {
			table_insert_sql += ", ";
		}
		table_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %d, %s)", table.id, table.uuid,
		                                       table.schema_id, SQLString(table.name));
		for (auto &column : table.columns) {
			if (!column_insert_sql.empty()) {
				column_insert_sql += ", ";
			}
			column_insert_sql +=
			    StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, %d, %s, %s, NULL)", column.id, table.id, column.id,
			                       SQLString(column.name), SQLString(column.type));
		}
	}
	if (!table_insert_sql.empty()) {
		// insert table entries
		table_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_table VALUES " + table_insert_sql;
		auto result = transaction.Query(commit_snapshot, table_insert_sql);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write new table to DuckLake: ");
		}
	}
	if (!column_insert_sql.empty()) {
		// insert column entries
		column_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_column VALUES " + column_insert_sql;
		auto result = transaction.Query(commit_snapshot, column_insert_sql);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write column information to DuckLake: ");
		}
	}
}

void DuckLakeMetadataManager::WriteNewDataFiles(DuckLakeSnapshot commit_snapshot,
                                                const vector<DuckLakeFileInfo> &new_files) {
	string data_file_insert_query;
	string column_stats_insert_query;

	for (auto &file : new_files) {
		if (!data_file_insert_query.empty()) {
			data_file_insert_query += ",";
		}
		data_file_insert_query += StringUtil::Format(
		    "(%d, %d, {SNAPSHOT_ID}, NULL, NULL, %s, 'parquet', %d, %d, %d, NULL)", file.id, file.table_id,
		    SQLString(file.file_name), file.row_count, file.file_size_bytes, file.footer_size);
		for (auto &column_stats : file.column_stats) {
			if (!column_stats_insert_query.empty()) {
				column_stats_insert_query += ",";
			}

			column_stats_insert_query += StringUtil::Format(
			    "(%d, %d, %d, NULL, %s, %s, NULL, %s, %s)", file.id, file.table_id, column_stats.column_id,
			    column_stats.value_count, column_stats.null_count, column_stats.min_val, column_stats.max_val);
		}
	}
	if (data_file_insert_query.empty()) {
		throw InternalException("No files found!?");
	}
	// insert the data files
	data_file_insert_query =
	    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_data_file VALUES %s", data_file_insert_query);
	auto result = transaction.Query(commit_snapshot, data_file_insert_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write data file information to DuckLake: ");
	}
	// insert the column stats
	column_stats_insert_query = StringUtil::Format(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_file_column_statistics VALUES %s", column_stats_insert_query);
	result = transaction.Query(commit_snapshot, column_stats_insert_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write column stats information to DuckLake: ");
	}
}

void DuckLakeMetadataManager::InsertSnapshot(DuckLakeSnapshot commit_snapshot) {
	auto result = transaction.Query(
	    commit_snapshot,
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES ({SNAPSHOT_ID}, NOW(), {SCHEMA_VERSION}, {NEXT_CATALOG_ID}, {NEXT_FILE_ID});)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new snapshot to DuckLake: ");
	}
}

string SQLStringOrNull(const string &str) {
	if (str.empty()) {
		return "NULL";
	}
	return KeywordHelper::WriteQuoted(str, '\'');
}

void DuckLakeMetadataManager::WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot,
                                                   const SnapshotChangeInfo &change_info) {
	// insert the snapshot changes
	auto query = StringUtil::Format(
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes VALUES ({SNAPSHOT_ID}, %s, %s, %s, %s, %s, %s, %s);)",
	    SQLStringOrNull(change_info.schemas_created), SQLStringOrNull(change_info.schemas_dropped),
	    SQLStringOrNull(change_info.tables_created), SQLStringOrNull(change_info.tables_dropped),
	    SQLStringOrNull(change_info.tables_altered), SQLStringOrNull(change_info.tables_inserted_into),
	    SQLStringOrNull(change_info.tables_deleted_from));
	auto result = transaction.Query(commit_snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new snapshot to DuckLake:");
	}
}

SnapshotChangeInfo DuckLakeMetadataManager::GetChangesMadeAfterSnapshot(DuckLakeSnapshot start_snapshot) {
	// get all changes made to the system after the snapshot was started
	auto result = transaction.Query(start_snapshot, R"(
	SELECT COALESCE(STRING_AGG(schemas_created), ''),
		   COALESCE(STRING_AGG(schemas_dropped), ''),
		   COALESCE(STRING_AGG(tables_created), ''),
		   COALESCE(STRING_AGG(tables_dropped), ''),
		   COALESCE(STRING_AGG(tables_altered), ''),
		   COALESCE(STRING_AGG(tables_inserted_into), ''),
		   COALESCE(STRING_AGG(tables_deleted_from), '')
	FROM {METADATA_CATALOG}.ducklake_snapshot_changes
	WHERE snapshot_id > {SNAPSHOT_ID}
	)");
	if (result->HasError()) {
		result->GetErrorObject().Throw(
		    "Failed to commit DuckLake transaction - failed to get snapshot changes for conflict resolution:");
	}
	// parse changes made by other transactions
	SnapshotChangeInfo change_info;
	for (auto &row : *result) {
		change_info.schemas_created = row.GetValue<string>(0);
		change_info.schemas_dropped = row.GetValue<string>(1);
		change_info.tables_created = row.GetValue<string>(2);
		change_info.tables_dropped = row.GetValue<string>(3);
	}
	return change_info;
}

unique_ptr<DuckLakeSnapshot> DuckLakeMetadataManager::GetSnapshot() {
	auto result = transaction.Query(
	    R"(SELECT snapshot_id, schema_version, next_catalog_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id = (SELECT MAX(snapshot_id) FROM {METADATA_CATALOG}.ducklake_snapshot);)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to query most recent snapshot for DuckLake: ");
	}
	auto chunk = result->Fetch();
	if (chunk->size() != 1) {
		throw InvalidInputException("Corrupt DuckLake - multiple snapshots returned from database");
	}

	auto snapshot_id = chunk->GetValue(0, 0).GetValue<idx_t>();
	auto schema_version = chunk->GetValue(1, 0).GetValue<idx_t>();
	auto next_catalog_id = chunk->GetValue(2, 0).GetValue<idx_t>();
	auto next_file_id = chunk->GetValue(3, 0).GetValue<idx_t>();
	return make_uniq<DuckLakeSnapshot>(snapshot_id, schema_version, next_catalog_id, next_file_id);
}

void DuckLakeMetadataManager::WriteNewPartitionKeys(DuckLakeSnapshot commit_snapshot,
                                                    const vector<DuckLakePartitionInfo> &new_partitions) {
	if (new_partitions.empty()) {
		return;
	}
	string old_partition_table_ids;
	string new_partition_values;
	string insert_partition_cols;
	for (auto &partition : new_partitions) {
		// set old partition data as no longer valid
		if (!old_partition_table_ids.empty()) {
			old_partition_table_ids += ", ";
		}
		old_partition_table_ids += to_string(partition.table_id);
		if (!partition.id.IsValid()) {
			// dropping partition data - we don't need to do anything
			return;
		}
		auto partition_id = partition.id.GetIndex();
		if (!new_partition_values.empty()) {
			new_partition_values += ", ";
		}
		new_partition_values +=
		    StringUtil::Format(R"((%d, %d, {SNAPSHOT_ID}, NULL);)", partition_id, partition.table_id);
		for (auto &field : partition.fields) {
			if (!insert_partition_cols.empty()) {
				insert_partition_cols += ", ";
			}
			if (field.transform.type != DuckLakeTransformType::IDENTITY) {
				throw NotImplementedException("FIXME: non-identity transform");
			}
			insert_partition_cols += StringUtil::Format("(%d, %d, %d, 'identity')", partition_id,
			                                            field.partition_key_index, field.column_id);
		}
	}
	// update old partition information for any tables that have been altered
	auto update_partition_query = StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_partition_info
SET end_snapshot = {SNAPSHOT_ID}
WHERE table_id IN (%s) AND end_snapshot IS NULL)",
	                                                 old_partition_table_ids);
	auto result = transaction.Query(commit_snapshot, update_partition_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update old partition information in DuckLake: ");
	}
	if (!new_partition_values.empty()) {
		new_partition_values = "INSERT INTO {METADATA_CATALOG}.ducklake_partition_info VALUES " + new_partition_values;
		auto result = transaction.Query(commit_snapshot, new_partition_values);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to insert new partition information in DuckLake: ");
		}
	}
	if (!insert_partition_cols.empty()) {
		insert_partition_cols =
		    "INSERT INTO {METADATA_CATALOG}.ducklake_partition_columns VALUES " + insert_partition_cols;

		auto result = transaction.Query(commit_snapshot, insert_partition_cols);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to insert new partition information in DuckLake:");
		}
	}
}

void DuckLakeMetadataManager::UpdateGlobalTableStats(const DuckLakeGlobalStatsInfo &stats) {
	auto &table_stats = stats.table_stats;
	string column_stats_values;
	for (auto &col_stats : stats.column_stats) {
		if (!column_stats_values.empty()) {
			column_stats_values += ",";
		}
		column_stats_values += StringUtil::Format("(%d, %d, %s, %s, %s)", stats.table_id, col_stats.column_id,
		                                          col_stats.contains_null, col_stats.min_val, col_stats.max_val);
	}

	if (!stats.initialized) {
		// stats have not been initialized yet - insert them
		auto result = transaction.Query(
		    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_table_stats VALUES (%d, %d, %d);",
		                       stats.table_id, table_stats.record_count, table_stats.table_size_bytes));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to insert stats information in DuckLake: ");
		}

		result = transaction.Query(StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_table_column_stats VALUES %s;", column_stats_values));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to insert stats information in DuckLake: ");
		}
		return;
	}
	// stats have been initialized - update them
	auto result = transaction.Query(StringUtil::Format(
	    "UPDATE {METADATA_CATALOG}.ducklake_table_stats SET record_count=%d, file_size_bytes=%d WHERE table_id=%d;",
	    table_stats.record_count, table_stats.table_size_bytes, stats.table_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update stats information in DuckLake: ");
	}
	result = transaction.Query(StringUtil::Format(R"(
WITH new_values(tid, cid, new_contains_null, new_min, new_max) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.ducklake_table_column_stats
SET contains_null=new_contains_null, min_value=new_min, max_value=new_max
FROM new_values
WHERE table_id=tid AND column_id=cid
)",
	                                              column_stats_values));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update stats information in DuckLake: ");
	}
}

} // namespace duckdb
