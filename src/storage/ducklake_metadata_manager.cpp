#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/types/blob.hpp"
#include "storage/ducklake_catalog.hpp"
#include "duckdb.hpp"

namespace duckdb {

DuckLakeMetadataManager::DuckLakeMetadataManager(DuckLakeTransaction &transaction) : transaction(transaction) {
}

DuckLakeMetadataManager::~DuckLakeMetadataManager() {
}

DuckLakeMetadataManager &DuckLakeMetadataManager::Get(DuckLakeTransaction &transaction) {
	return transaction.GetMetadataManager();
}

void DuckLakeMetadataManager::InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption) {
	string initialize_query;
	if (has_explicit_schema) {
		// if the schema is user provided create it
		initialize_query += "CREATE SCHEMA IF NOT EXISTS {METADATA_CATALOG};\n";
	}
	// we default to false unless explicitly specified otherwise
	string encryption_str = encryption == DuckLakeEncryption::ENCRYPTED ? "true" : "false";
	initialize_query += StringUtil::Format(R"(
CREATE TABLE {METADATA_CATALOG}.ducklake_metadata(key VARCHAR NOT NULL, value VARCHAR NOT NULL);
CREATE TABLE {METADATA_CATALOG}.ducklake_snapshot(snapshot_id BIGINT PRIMARY KEY, snapshot_time TIMESTAMPTZ, schema_version BIGINT, next_catalog_id BIGINT, next_file_id BIGINT);
CREATE TABLE {METADATA_CATALOG}.ducklake_snapshot_changes(snapshot_id BIGINT PRIMARY KEY, changes_made VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_schema(schema_id BIGINT PRIMARY KEY, schema_uuid UUID, begin_snapshot BIGINT, end_snapshot BIGINT, schema_name VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_table(table_id BIGINT, table_uuid UUID, begin_snapshot BIGINT, end_snapshot BIGINT, schema_id BIGINT, table_name VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_view(view_id BIGINT, view_uuid UUID, begin_snapshot BIGINT, end_snapshot BIGINT, schema_id BIGINT, view_name VARCHAR, dialect VARCHAR, sql VARCHAR, column_aliases VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_tag(object_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, key VARCHAR, value VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_column_tag(table_id BIGINT, column_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, key VARCHAR, value VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_data_file(data_file_id BIGINT PRIMARY KEY, table_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, file_order BIGINT, path VARCHAR, file_format VARCHAR, record_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, row_id_start BIGINT, partition_id BIGINT, encryption_key VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_file_column_statistics(data_file_id BIGINT, table_id BIGINT, column_id BIGINT, column_size_bytes BIGINT, value_count BIGINT, null_count BIGINT, nan_count BIGINT, min_value VARCHAR, max_value VARCHAR, contains_nan BOOLEAN);
CREATE TABLE {METADATA_CATALOG}.ducklake_delete_file(delete_file_id BIGINT PRIMARY KEY, table_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, data_file_id BIGINT, path VARCHAR, format VARCHAR, delete_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, encryption_key VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_column(column_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, table_id BIGINT, column_order BIGINT, column_name VARCHAR, column_type VARCHAR, initial_default VARCHAR, default_value VARCHAR, nulls_allowed BOOLEAN, parent_column BIGINT);
CREATE TABLE {METADATA_CATALOG}.ducklake_table_stats(table_id BIGINT, record_count BIGINT, next_row_id BIGINT, file_size_bytes BIGINT);
CREATE TABLE {METADATA_CATALOG}.ducklake_table_column_stats(table_id BIGINT, column_id BIGINT, contains_null BOOLEAN, contains_nan BOOLEAN, min_value VARCHAR, max_value VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_partition_info(partition_id BIGINT, table_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT);
CREATE TABLE {METADATA_CATALOG}.ducklake_partition_columns(partition_id BIGINT, partition_key_index BIGINT, column_id BIGINT, transform VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_file_partition_values(data_file_id BIGINT PRIMARY KEY, table_id BIGINT, partition_key_index BIGINT, partition_value VARCHAR);
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES (0, NOW(), 0, 1, 0);
INSERT INTO {METADATA_CATALOG}.ducklake_metadata VALUES ('version', '1'), ('created_by', 'DuckDB %s'), ('data_path', {DATA_PATH}), ('encryption', '%s');
INSERT INTO {METADATA_CATALOG}.ducklake_schema VALUES (0, UUID(), 0, NULL, 'main');
	)",
	                                       DuckDB::SourceID(), encryption_str);
	// TODO: add
	//	ducklake_sorting_info
	//	ducklake_sorting_column_info
	//	ducklake_macro
	auto result = transaction.Query(initialize_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to initialize DuckLake:");
	}
}

DuckLakeMetadata DuckLakeMetadataManager::LoadDuckLake() {
	auto result = transaction.Query("SELECT key, value FROM {METADATA_CATALOG}.ducklake_metadata");
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		error_obj.Throw("Failed to load existing DuckLake");
	}
	DuckLakeMetadata metadata;
	for (auto &row : *result) {
		DuckLakeTag tag;
		tag.key = row.GetValue<string>(0);
		tag.value = row.GetValue<string>(1);
		metadata.tags.push_back(std::move(tag));
	}
	return metadata;
}

bool AddChildColumn(vector<DuckLakeColumnInfo> &columns, FieldIndex parent_id, DuckLakeColumnInfo &column_info) {
	for (auto &col : columns) {
		if (col.id == parent_id) {
			col.children.push_back(std::move(column_info));
			return true;
		}
		if (AddChildColumn(col.children, parent_id, column_info)) {
			return true;
		}
	}
	return false;
}

vector<DuckLakeTag> LoadTags(const Value &tag_map) {
	vector<DuckLakeTag> result;
	for (auto &tag : ListValue::GetChildren(tag_map)) {
		auto &struct_children = StructValue::GetChildren(tag);
		if (struct_children[1].IsNull()) {
			continue;
		}
		DuckLakeTag tag_info;
		tag_info.key = struct_children[0].ToString();
		tag_info.value = struct_children[1].ToString();
		result.push_back(std::move(tag_info));
	}
	return result;
}

DuckLakeCatalogInfo DuckLakeMetadataManager::GetCatalogForSnapshot(DuckLakeSnapshot snapshot) {
	DuckLakeCatalogInfo catalog;
	// load the schema information
	auto result = transaction.Query(snapshot, R"(
SELECT schema_id, schema_uuid::VARCHAR, schema_name
FROM {METADATA_CATALOG}.ducklake_schema
WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get schema information from DuckLake: ");
	}
	for (auto &row : *result) {
		DuckLakeSchemaInfo schema;
		schema.id = SchemaIndex(row.GetValue<uint64_t>(0));
		schema.uuid = row.GetValue<string>(1);
		schema.name = row.GetValue<string>(2);
		catalog.schemas.push_back(std::move(schema));
	}

	// load the table information
	result = transaction.Query(snapshot, R"(
SELECT schema_id, tbl.table_id, table_uuid::VARCHAR, table_name,
	(
		SELECT LIST({'key': key, 'value': value})
		FROM {METADATA_CATALOG}.ducklake_tag tag
		WHERE object_id=table_id AND
		      {SNAPSHOT_ID} >= tag.begin_snapshot AND ({SNAPSHOT_ID} < tag.end_snapshot OR tag.end_snapshot IS NULL)
	) AS tag,
	col.column_id, column_name, column_type, initial_default, default_value, nulls_allowed, parent_column,
	(
		SELECT LIST({'key': key, 'value': value})
		FROM {METADATA_CATALOG}.ducklake_column_tag col_tag
		WHERE col_tag.table_id=tbl.table_id AND col_tag.column_id=col.column_id AND
		      {SNAPSHOT_ID} >= col_tag.begin_snapshot AND ({SNAPSHOT_ID} < col_tag.end_snapshot OR col_tag.end_snapshot IS NULL)
	) AS column_tags
FROM {METADATA_CATALOG}.ducklake_table tbl
LEFT JOIN {METADATA_CATALOG}.ducklake_column col USING (table_id)
WHERE {SNAPSHOT_ID} >= tbl.begin_snapshot AND ({SNAPSHOT_ID} < tbl.end_snapshot OR tbl.end_snapshot IS NULL)
  AND (({SNAPSHOT_ID} >= col.begin_snapshot AND ({SNAPSHOT_ID} < col.end_snapshot OR col.end_snapshot IS NULL)) OR column_id IS NULL)
ORDER BY table_id, parent_column NULLS FIRST, column_order
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get table information from DuckLake: ");
	}
	const idx_t COLUMN_INDEX_START = 5;
	auto &tables = catalog.tables;
	for (auto &row : *result) {
		auto table_id = TableIndex(row.GetValue<uint64_t>(1));

		// check if this column belongs to the current table or not
		if (tables.empty() || tables.back().id != table_id) {
			// new table
			DuckLakeTableInfo table_info;
			table_info.id = table_id;
			table_info.schema_id = SchemaIndex(row.GetValue<uint64_t>(0));
			table_info.uuid = row.GetValue<string>(2);
			table_info.name = row.GetValue<string>(3);
			if (!row.IsNull(4)) {
				auto tags = row.GetValue<Value>(4);
				table_info.tags = LoadTags(tags);
			}
			tables.push_back(std::move(table_info));
		}
		auto &table_entry = tables.back();
		if (row.GetValue<Value>(COLUMN_INDEX_START).IsNull()) {
			throw InvalidInputException("Failed to load DuckLake - Table entry \"%s\" does not have any columns",
			                            table_entry.name);
		}
		DuckLakeColumnInfo column_info;
		column_info.id = FieldIndex(row.GetValue<uint64_t>(COLUMN_INDEX_START));
		column_info.name = row.GetValue<string>(COLUMN_INDEX_START + 1);
		column_info.type = row.GetValue<string>(COLUMN_INDEX_START + 2);
		if (!row.IsNull(COLUMN_INDEX_START + 3)) {
			column_info.initial_default = Value(row.GetValue<string>(COLUMN_INDEX_START + 3));
		}
		if (!row.IsNull(COLUMN_INDEX_START + 4)) {
			column_info.default_value = Value(row.GetValue<string>(COLUMN_INDEX_START + 4));
		}
		column_info.nulls_allowed = row.GetValue<bool>(COLUMN_INDEX_START + 5);
		if (!row.IsNull(COLUMN_INDEX_START + 7)) {
			auto tags = row.GetValue<Value>(COLUMN_INDEX_START + 7);
			column_info.tags = LoadTags(tags);
		}

		if (row.IsNull(COLUMN_INDEX_START + 6)) {
			// base column - add the column to this table
			table_entry.columns.push_back(std::move(column_info));
		} else {
			auto parent_id = FieldIndex(row.GetValue<idx_t>(COLUMN_INDEX_START + 6));
			if (!AddChildColumn(table_entry.columns, parent_id, column_info)) {
				throw InvalidInputException("Failed to load DuckLake - Could not find parent column for column %s",
				                            column_info.name);
			}
		}
	}
	// load view information
	result = transaction.Query(snapshot, R"(
SELECT view_id, view_uuid, schema_id, view_name, dialect, sql, column_aliases,
	(
		SELECT LIST({'key': key, 'value': value})
		FROM {METADATA_CATALOG}.ducklake_tag tag
		WHERE object_id=view_id AND
		      {SNAPSHOT_ID} >= tag.begin_snapshot AND ({SNAPSHOT_ID} < tag.end_snapshot OR tag.end_snapshot IS NULL)
	) AS tag
FROM {METADATA_CATALOG}.ducklake_view view
WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < view.end_snapshot OR view.end_snapshot IS NULL)
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get partition information from DuckLake: ");
	}
	auto &views = catalog.views;
	for (auto &row : *result) {
		DuckLakeViewInfo view_info;
		view_info.id = TableIndex(row.GetValue<uint64_t>(0));
		view_info.uuid = row.GetValue<string>(1);
		view_info.schema_id = SchemaIndex(row.GetValue<uint64_t>(2));
		view_info.name = row.GetValue<string>(3);
		view_info.dialect = row.GetValue<string>(4);
		view_info.sql = row.GetValue<string>(5);
		view_info.column_aliases = DuckLakeUtil::ParseQuotedList(row.GetValue<string>(6));
		if (!row.IsNull(7)) {
			auto tags = row.GetValue<Value>(7);
			view_info.tags = LoadTags(tags);
		}
		views.push_back(std::move(view_info));
	}

	// load partition information
	result = transaction.Query(snapshot, R"(
SELECT partition_id, table_id, partition_key_index, column_id, transform
FROM {METADATA_CATALOG}.ducklake_partition_info part
JOIN {METADATA_CATALOG}.ducklake_partition_columns part_col USING (partition_id)
WHERE {SNAPSHOT_ID} >= part.begin_snapshot AND ({SNAPSHOT_ID} < part.end_snapshot OR part.end_snapshot IS NULL)
ORDER BY table_id, partition_id, partition_key_index
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get partition information from DuckLake: ");
	}
	auto &partitions = catalog.partitions;
	for (auto &row : *result) {
		auto partition_id = row.GetValue<uint64_t>(0);
		auto table_id = TableIndex(row.GetValue<uint64_t>(1));

		if (partitions.empty() || partitions.back().table_id != table_id) {
			DuckLakePartitionInfo partition_info;
			partition_info.id = partition_id;
			partition_info.table_id = table_id;
			partitions.push_back(std::move(partition_info));
		}
		auto &partition_entry = partitions.back();

		DuckLakePartitionFieldInfo partition_field;
		partition_field.partition_key_index = row.GetValue<uint64_t>(2);
		partition_field.column_id = row.GetValue<uint64_t>(3);
		partition_field.transform = row.GetValue<string>(4);
		partition_entry.fields.push_back(std::move(partition_field));
	}
	return catalog;
}

vector<DuckLakeGlobalStatsInfo> DuckLakeMetadataManager::GetGlobalTableStats(DuckLakeSnapshot snapshot) {
	// query the most recent stats
	auto result = transaction.Query(snapshot, R"(
SELECT table_id, column_id, record_count, next_row_id, file_size_bytes, contains_null, contains_nan, min_value, max_value
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats USING (table_id)
WHERE record_count IS NOT NULL AND file_size_bytes IS NOT NULL
ORDER BY table_id;
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get global stats information from DuckLake: ");
	}
	vector<DuckLakeGlobalStatsInfo> global_stats;
	for (auto &row : *result) {
		auto table_id = TableIndex(row.GetValue<uint64_t>(0));
		if (global_stats.empty() || global_stats.back().table_id != table_id) {
			// new stats
			DuckLakeGlobalStatsInfo new_entry;

			// set up the table-level stats
			new_entry.table_id = table_id;
			new_entry.initialized = true;
			new_entry.record_count = row.GetValue<uint64_t>(2);
			new_entry.next_row_id = row.GetValue<uint64_t>(3);
			new_entry.table_size_bytes = row.GetValue<uint64_t>(4);
			global_stats.push_back(std::move(new_entry));
		}
		auto &stats_entry = global_stats.back();

		DuckLakeGlobalColumnStatsInfo column_stats;
		column_stats.column_id = FieldIndex(row.GetValue<uint64_t>(1));
		static constexpr const idx_t COLUMN_STATS_START = 5;
		if (row.IsNull(COLUMN_STATS_START)) {
			column_stats.has_contains_null = false;
		} else {
			column_stats.has_contains_null = true;
			column_stats.contains_null = row.GetValue<bool>(COLUMN_STATS_START);
		}
		if (row.IsNull(COLUMN_STATS_START + 1)) {
			column_stats.has_contains_nan = false;
		} else {
			column_stats.has_contains_nan = true;
			column_stats.contains_nan = row.GetValue<bool>(COLUMN_STATS_START + 1);
		}
		if (row.IsNull(COLUMN_STATS_START + 2)) {
			column_stats.has_min = false;
		} else {
			column_stats.has_min = true;
			column_stats.min_val = row.GetValue<string>(COLUMN_STATS_START + 2);
		}
		if (row.IsNull(COLUMN_STATS_START + 3)) {
			column_stats.has_max = false;
		} else {
			column_stats.has_max = true;
			column_stats.max_val = row.GetValue<string>(COLUMN_STATS_START + 3);
		}

		stats_entry.column_stats.push_back(std::move(column_stats));
	}
	return global_stats;
}

string DuckLakeMetadataManager::GetFileSelectList(const string &prefix) {
	auto result = StringUtil::Replace("{PREFIX}.path, {PREFIX}.file_size_bytes, {PREFIX}.footer_size", "{PREFIX}", prefix);
	if (IsEncrypted()) {
		result += ", " + prefix + ".encryption_key";
	}
	return result;
}

template<class T>
DuckLakeFileData ReadDataFile(T &row, idx_t &col_idx, bool is_encrypted) {
	DuckLakeFileData data;
	if (row.IsNull(col_idx)) {
		// file is not there
		col_idx += 3;
		if (is_encrypted) {
			col_idx++;
		}
		return data;
	}
	data.path = row.template GetValue<string>(col_idx++);
	data.file_size_bytes = row.template GetValue<idx_t>(col_idx++);
	data.footer_size = row.template GetValue<idx_t>(col_idx++);
	if (is_encrypted) {
		if (row.IsNull(col_idx)) {
			throw InvalidInputException("Database is encrypted, but file %s does not have an encryption key", data.path);
		}
		data.encryption_key = Blob::FromBase64(row.template GetValue<string>(col_idx++));
	}
	return data;
}

vector<DuckLakeFileListEntry> DuckLakeMetadataManager::GetFilesForTable(DuckLakeSnapshot snapshot, TableIndex table_id, const string &filter) {
	string select_list = GetFileSelectList("data") + ", data.row_id_start, " + GetFileSelectList("del");
	auto query = StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.ducklake_data_file data
LEFT JOIN (
    SELECT *
    FROM {METADATA_CATALOG}.ducklake_delete_file
    WHERE table_id=%d  AND {SNAPSHOT_ID} >= begin_snapshot
          AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
    ) del USING (data_file_id)
WHERE data.table_id=%d AND {SNAPSHOT_ID} >= data.begin_snapshot AND ({SNAPSHOT_ID} < data.end_snapshot OR data.end_snapshot IS NULL)
		)", select_list, table_id.index, table_id.index);
	if (!filter.empty()) {
		query += "\nAND " + filter;
	}

	auto result = transaction.Query(snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get data file list from DuckLake: ");
	}
	vector<DuckLakeFileListEntry> files;
	for (auto &row : *result) {
		DuckLakeFileListEntry file_entry;
		idx_t col_idx = 0;
		file_entry.file = ReadDataFile(row, col_idx, IsEncrypted());
		file_entry.row_id_start = row.GetValue<idx_t>(col_idx++);
		file_entry.delete_file = ReadDataFile(row, col_idx, IsEncrypted());
		files.push_back(std::move(file_entry));
	}
	return files;
}

vector<DuckLakeFileListExtendedEntry> DuckLakeMetadataManager::GetExtendedFilesForTable(DuckLakeSnapshot snapshot, TableIndex table_id,
const string &filter) {
	string select_list = GetFileSelectList("data") + ", data.row_id_start, " + GetFileSelectList("del");
	auto query = StringUtil::Format(R"(
SELECT data.data_file_id, del.delete_file_id, data.record_count, %s
FROM {METADATA_CATALOG}.ducklake_data_file data
LEFT JOIN (
	SELECT *
    FROM {METADATA_CATALOG}.ducklake_delete_file
    WHERE table_id=%d  AND {SNAPSHOT_ID} >= begin_snapshot
          AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
    ) del USING (data_file_id)
WHERE data.table_id=%d AND {SNAPSHOT_ID} >= data.begin_snapshot AND ({SNAPSHOT_ID} < data.end_snapshot OR data.end_snapshot IS NULL)
		)", select_list, table_id.index, table_id.index);
	if (!filter.empty()) {
		query += "\nAND " + filter;
	}

	auto result = transaction.Query(snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get extended data file list from DuckLake: ");
	}
	vector<DuckLakeFileListExtendedEntry> files;
	for (auto &row : *result) {
		DuckLakeFileListExtendedEntry file_entry;
		file_entry.file_id = DataFileIndex(row.GetValue<idx_t>(0));
		if (!row.IsNull(1)) {
			file_entry.delete_file_id = DataFileIndex(row.GetValue<idx_t>(1));
		}
		file_entry.row_count = row.GetValue<idx_t>(2);
		idx_t col_idx = 3;
		file_entry.file = ReadDataFile(row, col_idx, IsEncrypted());
		file_entry.row_id_start = row.GetValue<idx_t>(col_idx++);
		file_entry.delete_file = ReadDataFile(row, col_idx, IsEncrypted());
		files.push_back(std::move(file_entry));
	}
	return files;

}

template <class T>
void DuckLakeMetadataManager::FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name,
                                        const string &id_name, const set<T> &dropped_entries) {
    if (dropped_entries.empty()) {
	    return;
    }
	string dropped_id_list;
	for (auto &dropped_id : dropped_entries) {
		if (!dropped_id_list.empty()) {
			dropped_id_list += ", ";
		}
		dropped_id_list += to_string(dropped_id.index);
	}
	auto dropped_id_query =
	    StringUtil::Format(R"(UPDATE {METADATA_CATALOG}.%s SET end_snapshot = {SNAPSHOT_ID} WHERE end_snapshot IS NULL AND %s IN (%s);)",
	                       metadata_table_name, id_name, dropped_id_list);
	auto result = transaction.Query(commit_snapshot, dropped_id_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write drop information to DuckLake:");
	}
}

void DuckLakeMetadataManager::DropSchemas(DuckLakeSnapshot commit_snapshot, set<SchemaIndex> ids) {
	FlushDrop(commit_snapshot, "ducklake_schema", "schema_id", ids);
}

void DuckLakeMetadataManager::DropTables(DuckLakeSnapshot commit_snapshot, set<TableIndex> ids) {
	FlushDrop(commit_snapshot, "ducklake_table", "table_id", ids);
}

void DuckLakeMetadataManager::DropViews(DuckLakeSnapshot commit_snapshot, set<TableIndex> ids) {
	FlushDrop(commit_snapshot, "ducklake_view", "view_id", ids);
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
		auto schema_id = new_schema.id.index;
		schema_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %s)", schema_id, new_schema.uuid,
		                                        SQLString(new_schema.name));
	}
	schema_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_schema VALUES " + schema_insert_sql;
	auto result = transaction.Query(commit_snapshot, schema_insert_sql);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new schemas to DuckLake: ");
	}
}

void ColumnToSQLRecursive(const DuckLakeColumnInfo &column, TableIndex table_id, optional_idx parent, string &result) {
	if (!result.empty()) {
		result += ",";
	}
	string parent_idx = parent.IsValid() ? to_string(parent.GetIndex()) : "NULL";
	string initial_default = !column.initial_default.IsNull() ? KeywordHelper::WriteQuoted(column.initial_default.ToString(), '\'') : "NULL";
	string default_val = !column.default_value.IsNull() ? KeywordHelper::WriteQuoted(column.default_value.ToString(), '\'') : "NULL";
	auto column_id = column.id.index;
	auto column_order = column_id;
	result += StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, %d, %s, %s, %s, %s, %d, %s)", column_id, table_id.index,
	                             column_order, SQLString(column.name), SQLString(column.type), initial_default, default_val,
	                             column.nulls_allowed ? 1 : 0, parent_idx);
	for (auto &child : column.children) {
		ColumnToSQLRecursive(child, table_id, column_id, result);
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
		auto schema_id = table.schema_id.index;
		table_insert_sql +=
		    StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %d, %s)", table.id.index, table.uuid, schema_id,
		                       SQLString(table.name));
		for (auto &column : table.columns) {
			ColumnToSQLRecursive(column, table.id, optional_idx(), column_insert_sql);
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

void DuckLakeMetadataManager::WriteDroppedColumns(DuckLakeSnapshot commit_snapshot,
                                                  const vector<DuckLakeDroppedColumn> &dropped_columns) {
	if (dropped_columns.empty()) {
		return;
	}
	string dropped_cols;
	for (auto &dropped_col : dropped_columns) {
		if (!dropped_cols.empty()) {
			dropped_cols += ", ";
		}
		dropped_cols += StringUtil::Format("(%d, %d)", dropped_col.table_id.index, dropped_col.field_id.index);
	}
	// overwrite the snapshot for the old columns
	auto result = transaction.Query(commit_snapshot, StringUtil::Format(R"(
WITH dropped_cols(tid, cid) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.ducklake_column
SET end_snapshot = {SNAPSHOT_ID}
FROM dropped_cols
WHERE table_id=tid AND column_id=cid
)",
	                                                                    dropped_cols));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to drop columns in DuckLake: ");
	}
}

void DuckLakeMetadataManager::WriteNewColumns(DuckLakeSnapshot commit_snapshot,
                                              const vector<DuckLakeNewColumn> &new_columns) {
	if (new_columns.empty()) {
		return;
	}
	string column_insert_sql;
	for (auto &new_col : new_columns) {
		ColumnToSQLRecursive(new_col.column_info, new_col.table_id, new_col.parent_idx, column_insert_sql);
	}

	// insert column entries
	column_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_column VALUES " + column_insert_sql;
	auto result = transaction.Query(commit_snapshot, column_insert_sql);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write column information to DuckLake: ");
	}
}

void DuckLakeMetadataManager::WriteNewViews(DuckLakeSnapshot commit_snapshot,
                                            const vector<DuckLakeViewInfo> &new_views) {
	string view_insert_sql;
	for (auto &view : new_views) {
		if (!view_insert_sql.empty()) {
			view_insert_sql += ", ";
		}
		auto schema_id = view.schema_id.index;
		view_insert_sql +=
		    StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %d, %s, %s, %s, %s)", view.id.index, view.uuid,
		                       schema_id, SQLString(view.name), SQLString(view.dialect), SQLString(view.sql),
		                       SQLString(DuckLakeUtil::ToQuotedList(view.column_aliases)));
	}
	if (!view_insert_sql.empty()) {
		// insert table entries
		view_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_view VALUES " + view_insert_sql;
		auto result = transaction.Query(commit_snapshot, view_insert_sql);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write new view to DuckLake: ");
		}
	}
}

void DuckLakeMetadataManager::WriteNewDataFiles(DuckLakeSnapshot commit_snapshot,
                                                const vector<DuckLakeFileInfo> &new_files) {
	string data_file_insert_query;
	string column_stats_insert_query;
	string partition_insert_query;

	for (auto &file : new_files) {
		if (!data_file_insert_query.empty()) {
			data_file_insert_query += ",";
		}
		auto row_id = file.row_id_start.IsValid() ? to_string(file.row_id_start.GetIndex()) : "NULL";
		auto partition_id = file.partition_id.IsValid() ? to_string(file.partition_id.GetIndex()) : "NULL";
		auto data_file_index = file.id.index;
		auto table_id = file.table_id.index;
		auto encryption_key = file.encryption_key.empty() ? "NULL" : "'" + Blob::ToBase64(string_t(file.encryption_key)) + "'";
		data_file_insert_query += StringUtil::Format(
		    "(%d, %d, {SNAPSHOT_ID}, NULL, NULL, %s, 'parquet', %d, %d, %d, %s, %s, %s)", data_file_index, table_id,
		    SQLString(file.file_name), file.row_count, file.file_size_bytes, file.footer_size, row_id, partition_id, encryption_key);
		for (auto &column_stats : file.column_stats) {
			if (!column_stats_insert_query.empty()) {
				column_stats_insert_query += ",";
			}
			auto column_id = column_stats.column_id.index;
			column_stats_insert_query +=
			    StringUtil::Format("(%d, %d, %d, NULL, %s, %s, %s, %s, %s, %s)", data_file_index, table_id, column_id,
			                       column_stats.value_count, column_stats.null_count, column_stats.column_size_bytes,
			                       column_stats.min_val, column_stats.max_val, column_stats.contains_nan);
		}
		if (file.partition_id.IsValid() == file.partition_values.empty()) {
			throw InternalException("File should either not be partitioned, or have partition values");
		}
		for (auto &part_val : file.partition_values) {
			if (!partition_insert_query.empty()) {
				partition_insert_query += ",";
			}
			partition_insert_query +=
			    StringUtil::Format("(%d, %d, %d, %s)", data_file_index, table_id, part_val.partition_column_idx,
			                       SQLString(part_val.partition_value));
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
	if (!partition_insert_query.empty()) {
		// insert the partition values
		partition_insert_query = StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_file_partition_values VALUES %s", partition_insert_query);
		result = transaction.Query(commit_snapshot, partition_insert_query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write partition value information to DuckLake: ");
		}
	}
}

void DuckLakeMetadataManager::DropDataFiles(DuckLakeSnapshot commit_snapshot, const set<DataFileIndex> &dropped_files) {
	FlushDrop(commit_snapshot, "ducklake_data_file", "data_file_id", dropped_files);
}

void DuckLakeMetadataManager::DropDeleteFiles(DuckLakeSnapshot commit_snapshot, const set<DataFileIndex> &dropped_files) {
	FlushDrop(commit_snapshot, "ducklake_delete_file", "data_file_id", dropped_files);
}

void DuckLakeMetadataManager::WriteNewDeleteFiles(DuckLakeSnapshot commit_snapshot,
                                                const vector<DuckLakeDeleteFileInfo> &new_files) {
	string delete_file_insert_query;
	for (auto &file : new_files) {
		if (!delete_file_insert_query.empty()) {
			delete_file_insert_query += ",";
		}
		auto delete_file_index = file.id.index;
		auto table_id = file.table_id.index;
		auto data_file_index = file.data_file_id.index;
		auto encryption_key = file.encryption_key.empty() ? "NULL" : "'" + Blob::ToBase64(string_t(file.encryption_key)) + "'";
		delete_file_insert_query += StringUtil::Format(
			"(%d, %d, {SNAPSHOT_ID}, NULL, %d, %s, 'parquet', %d, %d, %d, %s)", delete_file_index, table_id, data_file_index,
			SQLString(file.path), file.delete_count, file.file_size_bytes, file.footer_size, encryption_key);
	}

	// insert the data files
	delete_file_insert_query =
		StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_delete_file VALUES %s", delete_file_insert_query);
	auto result = transaction.Query(commit_snapshot, delete_file_insert_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write delete file information to DuckLake: ");
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
	auto query =
	    StringUtil::Format(R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes VALUES ({SNAPSHOT_ID}, %s);)",
	                       SQLStringOrNull(change_info.changes_made));
	auto result = transaction.Query(commit_snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new snapshot to DuckLake:");
	}
}

SnapshotChangeInfo DuckLakeMetadataManager::GetChangesMadeAfterSnapshot(DuckLakeSnapshot start_snapshot) {
	// get all changes made to the system after the snapshot was started
	auto result = transaction.Query(start_snapshot, R"(
	SELECT COALESCE(STRING_AGG(changes_made), '')
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
		change_info.changes_made = row.GetValue<string>(0);
	}
	return change_info;
}

unique_ptr<DuckLakeSnapshot> GetSnapshotInternal(DataChunk &chunk) {
	if (chunk.size() != 1) {
		throw InvalidInputException("Corrupt DuckLake - multiple snapshots returned from database");
	}
	auto snapshot_id = chunk.GetValue(0, 0).GetValue<idx_t>();
	auto schema_version = chunk.GetValue(1, 0).GetValue<idx_t>();
	auto next_catalog_id = chunk.GetValue(2, 0).GetValue<idx_t>();
	auto next_file_id = chunk.GetValue(3, 0).GetValue<idx_t>();
	return make_uniq<DuckLakeSnapshot>(snapshot_id, schema_version, next_catalog_id, next_file_id);
}

unique_ptr<DuckLakeSnapshot> DuckLakeMetadataManager::GetSnapshot() {
	auto result = transaction.Query(
	    R"(SELECT snapshot_id, schema_version, next_catalog_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id = (SELECT MAX(snapshot_id) FROM {METADATA_CATALOG}.ducklake_snapshot);)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to query most recent snapshot for DuckLake: ");
	}
	auto chunk = result->Fetch();
	if (!chunk) {
		throw InvalidInputException("No snapshot found in DuckLake");
	}
	return GetSnapshotInternal(*chunk);
}

unique_ptr<DuckLakeSnapshot> DuckLakeMetadataManager::GetSnapshot(BoundAtClause &at_clause) {
	auto &unit = at_clause.Unit();
	auto &val = at_clause.GetValue();
	unique_ptr<QueryResult> result;
	if (StringUtil::CIEquals(unit, "version")) {
		result = transaction.Query(StringUtil::Format(R"(
SELECT snapshot_id, schema_version, next_catalog_id, next_file_id
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = %llu;)",
		                                              val.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>()));
	} else if (StringUtil::CIEquals(unit, "timestamp")) {
		result = transaction.Query(StringUtil::Format(R"(
SELECT snapshot_id, schema_version, next_catalog_id, next_file_id
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = (
	SELECT MAX_BY(snapshot_id, snapshot_time)
	FROM {METADATA_CATALOG}.ducklake_snapshot
	WHERE snapshot_time < %s);)",
		                                              val.DefaultCastAs(LogicalType::TIMESTAMP).ToSQLString()));
	} else {
		throw InvalidInputException("Unsupported AT clause unit - %s", unit);
	}
	if (result->HasError()) {
		result->GetErrorObject().Throw(StringUtil::Format(
		    "Failed to query snapshot at %s %s for DuckLake: ", StringUtil::Lower(unit), val.ToString()));
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		throw InvalidInputException("No snapshot found at %s %s", StringUtil::Lower(unit), val.ToString());
	}
	return GetSnapshotInternal(*chunk);
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
		old_partition_table_ids += to_string(partition.table_id.index);
		if (!partition.id.IsValid()) {
			// dropping partition data - we don't need to do anything
			return;
		}
		auto partition_id = partition.id.GetIndex();
		if (!new_partition_values.empty()) {
			new_partition_values += ", ";
		}
		new_partition_values +=
		    StringUtil::Format(R"((%d, %d, {SNAPSHOT_ID}, NULL);)", partition_id, partition.table_id.index);
		for (auto &field : partition.fields) {
			if (!insert_partition_cols.empty()) {
				insert_partition_cols += ", ";
			}
			insert_partition_cols += StringUtil::Format("(%d, %d, %d, %s)", partition_id, field.partition_key_index,
			                                            field.column_id, SQLString(field.transform));
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

void DuckLakeMetadataManager::WriteNewTags(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeTagInfo> &new_tags) {
	if (new_tags.empty()) {
		return;
	}
	// update old tags (if there were any)
	// get a list of all tags
	string tags_list;
	for (auto &tag : new_tags) {
		if (!tags_list.empty()) {
			tags_list += ", ";
		}
		tags_list += StringUtil::Format("(%d, %s)", tag.id, SQLString(tag.key));
	}
	// overwrite the snapshot for the old tags
	auto result = transaction.Query(commit_snapshot, StringUtil::Format(R"(
WITH overwritten_tags(tid, key) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.ducklake_tag
SET end_snapshot = {SNAPSHOT_ID}
FROM overwritten_tags
WHERE object_id=tid
)",
	                                                                    tags_list));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update tag information in DuckLake: ");
	}
	// now insert the new tags
	string new_tag_query;
	for (auto &tag : new_tags) {
		if (!new_tag_query.empty()) {
			new_tag_query += ", ";
		}
		new_tag_query += StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %s, %s)", tag.id, SQLString(tag.key),
		                                    tag.value.ToSQLString());
	}

	new_tag_query = "INSERT INTO {METADATA_CATALOG}.ducklake_tag VALUES " + new_tag_query;

	result = transaction.Query(commit_snapshot, new_tag_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to insert new tag information in DuckLake:");
	}
}

void DuckLakeMetadataManager::WriteNewColumnTags(DuckLakeSnapshot commit_snapshot,
                                                 const vector<DuckLakeColumnTagInfo> &new_tags) {
	if (new_tags.empty()) {
		return;
	}
	// update old tags (if there were any)
	// get a list of all tags
	string tags_list;
	for (auto &tag : new_tags) {
		if (!tags_list.empty()) {
			tags_list += ", ";
		}
		tags_list += StringUtil::Format("(%d, %d, %s)", tag.table_id.index, tag.field_index.index, SQLString(tag.key));
	}
	// overwrite the snapshot for the old tags
	auto result = transaction.Query(commit_snapshot, StringUtil::Format(R"(
WITH overwritten_tags(tid, cid, key) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.ducklake_column_tag
SET end_snapshot = {SNAPSHOT_ID}
FROM overwritten_tags
WHERE table_id=tid AND column_id=cid
)",
	                                                                    tags_list));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update column tag information in DuckLake: ");
	}
	// now insert the new tags
	string new_tag_query;
	for (auto &tag : new_tags) {
		if (!new_tag_query.empty()) {
			new_tag_query += ", ";
		}
		new_tag_query += StringUtil::Format("(%d, %d, {SNAPSHOT_ID}, NULL, %s, %s)", tag.table_id.index,
		                                    tag.field_index.index, SQLString(tag.key), tag.value.ToSQLString());
	}

	new_tag_query = "INSERT INTO {METADATA_CATALOG}.ducklake_column_tag VALUES " + new_tag_query;

	result = transaction.Query(commit_snapshot, new_tag_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to insert new column tag information in DuckLake:");
	}
}

void DuckLakeMetadataManager::UpdateGlobalTableStats(const DuckLakeGlobalStatsInfo &stats) {
	string column_stats_values;
	for (auto &col_stats : stats.column_stats) {
		if (!column_stats_values.empty()) {
			column_stats_values += ",";
		}
		string contains_null;
		if (col_stats.has_contains_null) {
			contains_null = col_stats.contains_null ? "true" : "false";
		} else {
			contains_null = "NULL";
		}
		string contains_nan;
		if (col_stats.has_contains_nan) {
			contains_nan = col_stats.contains_nan ? "true" : "false";
		} else {
			contains_nan = "NULL";
		}
		string min_val = col_stats.has_min ? DuckLakeUtil::SQLLiteralToString(col_stats.min_val) : "NULL";
		string max_val = col_stats.has_max ? DuckLakeUtil::SQLLiteralToString(col_stats.max_val) : "NULL";
		column_stats_values +=
		    StringUtil::Format("(%d, %d, %s, %s, %s, %s)", stats.table_id.index, col_stats.column_id.index,
		                       contains_null, contains_nan, min_val, max_val);
	}

	if (!stats.initialized) {
		// stats have not been initialized yet - insert them
		auto result = transaction.Query(
		    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_table_stats VALUES (%d, %d, %d, %d);",
		                       stats.table_id.index, stats.record_count, stats.next_row_id, stats.table_size_bytes));
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
	    "UPDATE {METADATA_CATALOG}.ducklake_table_stats SET record_count=%d, file_size_bytes=%d, next_row_id=%d WHERE table_id=%d;",
	    stats.record_count, stats.table_size_bytes, stats.next_row_id, stats.table_id.index));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update stats information in DuckLake: ");
	}
	result = transaction.Query(StringUtil::Format(R"(
WITH new_values(tid, cid, new_contains_null, new_contains_nan, new_min, new_max) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.ducklake_table_column_stats
SET contains_null=new_contains_null, contains_nan=new_contains_nan, min_value=new_min, max_value=new_max
FROM new_values
WHERE table_id=tid AND column_id=cid
)",
	                                              column_stats_values));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update stats information in DuckLake: ");
	}
}

vector<DuckLakeSnapshotInfo> DuckLakeMetadataManager::GetAllSnapshots() {
	auto res = transaction.Query(R"(
SELECT snapshot_id, snapshot_time, schema_version, changes_made
FROM {METADATA_CATALOG}.ducklake_snapshot
LEFT JOIN {METADATA_CATALOG}.ducklake_snapshot_changes USING (snapshot_id)
ORDER BY snapshot_id
)");
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to get snapshot information from DuckLake: ");
	}
	vector<DuckLakeSnapshotInfo> snapshots;
	for (auto &row : *res) {
		DuckLakeSnapshotInfo snapshot_info;
		snapshot_info.id = row.GetValue<idx_t>(0);
		snapshot_info.time = row.GetValue<timestamp_tz_t>(1);
		snapshot_info.schema_version = row.GetValue<idx_t>(2);
		snapshot_info.change_info.changes_made = row.IsNull(3) ? string() : row.GetValue<string>(3);

		snapshots.push_back(std::move(snapshot_info));
	}
	return snapshots;
}

idx_t DuckLakeMetadataManager::GetNextColumnId(TableIndex table_id) {
	auto result = transaction.Query(StringUtil::Format(R"(
	SELECT MAX(column_id)
	FROM {METADATA_CATALOG}.ducklake_column
	WHERE table_id=%d
)", table_id.index));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get next column id in DuckLake: ");
	}
	for (auto &row : *result) {
		if (row.IsNull(0)) {
			break;
		}
		return row.GetValue<idx_t>(00) + 1;
	}
	throw InternalException("Invalid result for GetNextColumnId");
}

bool DuckLakeMetadataManager::IsEncrypted() const {
	return transaction.GetCatalog().Encryption() == DuckLakeEncryption::ENCRYPTED;
}

} // namespace duckdb
