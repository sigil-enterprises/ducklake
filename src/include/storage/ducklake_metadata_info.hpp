//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_metadata_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/types/value.hpp"
#include "common/index.hpp"

namespace duckdb {

struct DuckLakeTag {
	string key;
	string value;
};

struct DuckLakeMetadata {
	vector<DuckLakeTag> tags;
};

struct DuckLakeSchemaInfo {
	SchemaIndex id;
	string uuid;
	string name;
	vector<DuckLakeTag> tags;
};

struct DuckLakeColumnInfo {
	FieldIndex id;
	string name;
	string type;
	Value initial_default;
	Value default_value;
	bool nulls_allowed;
	vector<DuckLakeColumnInfo> children;
	vector<DuckLakeTag> tags;
};

struct DuckLakeTableInfo {
	TableIndex id;
	SchemaIndex schema_id;
	string uuid;
	string name;
	vector<DuckLakeColumnInfo> columns;
	vector<DuckLakeTag> tags;
};

struct DuckLakeColumnStatsInfo {
	FieldIndex column_id;
	string value_count;
	string null_count;
	string column_size_bytes;
	string min_val;
	string max_val;
	string contains_nan;
};

struct DuckLakeFilePartitionInfo {
	idx_t partition_column_idx;
	string partition_value;
};

struct DuckLakeFileInfo {
	DataFileIndex id;
	TableIndex table_id;
	string file_name;
	idx_t row_count;
	idx_t file_size_bytes;
	idx_t footer_size;
	optional_idx partition_id;
	vector<DuckLakeColumnStatsInfo> column_stats;
	vector<DuckLakeFilePartitionInfo> partition_values;
};

struct DuckLakeDeleteFileInfo {
	DataFileIndex id;
	TableIndex table_id;
	DataFileIndex data_file_id;
	string path;
	idx_t delete_count;
	idx_t file_size_bytes;
	idx_t footer_size;
};

struct DuckLakePartitionFieldInfo {
	idx_t partition_key_index = 0;
	idx_t column_id;
	string transform;
};

struct DuckLakePartitionInfo {
	optional_idx id;
	TableIndex table_id;
	vector<DuckLakePartitionFieldInfo> fields;
};

struct DuckLakeGlobalColumnStatsInfo {
	FieldIndex column_id;

	bool contains_null;
	bool has_contains_null;

	bool contains_nan;
	bool has_contains_nan;

	string min_val;
	bool has_min;

	string max_val;
	bool has_max;
};

struct DuckLakeGlobalStatsInfo {
	TableIndex table_id;
	bool initialized;
	idx_t record_count;
	idx_t table_size_bytes;
	vector<DuckLakeGlobalColumnStatsInfo> column_stats;
};

struct SnapshotChangeInfo {
	string changes_made;
};

struct DuckLakeSnapshotInfo {
	idx_t id;
	timestamp_tz_t time;
	idx_t schema_version;
	SnapshotChangeInfo change_info;
};

struct DuckLakeViewInfo {
	TableIndex id;
	SchemaIndex schema_id;
	string uuid;
	string name;
	string dialect;
	vector<string> column_aliases;
	string sql;
	vector<DuckLakeTag> tags;
};

struct DuckLakeTagInfo {
	idx_t id;
	string key;
	Value value;
};

struct DuckLakeColumnTagInfo {
	TableIndex table_id;
	FieldIndex field_index;
	string key;
	Value value;
};

struct DuckLakeDroppedColumn {
	TableIndex table_id;
	FieldIndex field_id;
};

struct DuckLakeNewColumn {
	TableIndex table_id;
	DuckLakeColumnInfo column_info;
	optional_idx parent_idx;
};

struct DuckLakeCatalogInfo {
	vector<DuckLakeSchemaInfo> schemas;
	vector<DuckLakeTableInfo> tables;
	vector<DuckLakeViewInfo> views;
	vector<DuckLakePartitionInfo> partitions;
};

} // namespace duckdb