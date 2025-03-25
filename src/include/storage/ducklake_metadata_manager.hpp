//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_metadata_manager.hpp
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
#include "common/ducklake_snapshot.hpp"
#include "storage/ducklake_partition_data.hpp"
#include "storage/ducklake_stats.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "common/index.hpp"

namespace duckdb {
class DuckLakeCatalogSet;
class DuckLakeSchemaEntry;
class DuckLakeTableEntry;
class DuckLakeTransaction;
class BoundAtClause;

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
	string min_val;
	string max_val;
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
};

struct DuckLakeCatalogInfo {
	vector<DuckLakeSchemaInfo> schemas;
	vector<DuckLakeTableInfo> tables;
	vector<DuckLakeViewInfo> views;
	vector<DuckLakePartitionInfo> partitions;
};

// The DuckLake metadata manger is the communication layer between the system and the metadata catalog
class DuckLakeMetadataManager {
public:
	DuckLakeMetadataManager(DuckLakeTransaction &transaction);
	virtual ~DuckLakeMetadataManager();

	DuckLakeMetadataManager &Get(DuckLakeTransaction &transaction);

	//! Initialize a new DuckLake
	virtual void InitializeDuckLake(bool has_explicit_schema);
	virtual DuckLakeMetadata LoadDuckLake();
	//! Get the catalog information for a specific snapshot
	virtual DuckLakeCatalogInfo GetCatalogForSnapshot(DuckLakeSnapshot snapshot);
	virtual vector<DuckLakeGlobalStatsInfo> GetGlobalTableStats(DuckLakeSnapshot snapshot);
	virtual void DropSchemas(DuckLakeSnapshot commit_snapshot, set<SchemaIndex> ids);
	virtual void DropTables(DuckLakeSnapshot commit_snapshot, set<TableIndex> ids);
	virtual void DropViews(DuckLakeSnapshot commit_snapshot, set<TableIndex> ids);
	virtual void WriteNewSchemas(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeSchemaInfo> &new_schemas);
	virtual void WriteNewTables(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeTableInfo> &new_tables);
	virtual void WriteNewViews(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeViewInfo> &new_views);
	virtual void WriteNewPartitionKeys(DuckLakeSnapshot commit_snapshot,
	                                   const vector<DuckLakePartitionInfo> &new_partitions);
	virtual void WriteDroppedColumns(DuckLakeSnapshot commit_snapshot,
	                                 const vector<DuckLakeDroppedColumn> &dropped_columns);
	virtual void WriteNewColumns(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeNewColumn> &new_columns);
	virtual void WriteNewTags(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeTagInfo> &new_tags);
	virtual void WriteNewColumnTags(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeColumnTagInfo> &new_tags);
	virtual void WriteNewDataFiles(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeFileInfo> &new_files);
	virtual void InsertSnapshot(DuckLakeSnapshot commit_snapshot);
	virtual void WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot, const SnapshotChangeInfo &change_info);
	virtual void UpdateGlobalTableStats(const DuckLakeGlobalStatsInfo &stats);
	virtual SnapshotChangeInfo GetChangesMadeAfterSnapshot(DuckLakeSnapshot start_snapshot);
	virtual unique_ptr<DuckLakeSnapshot> GetSnapshot();
	virtual unique_ptr<DuckLakeSnapshot> GetSnapshot(BoundAtClause &at_clause);

	virtual vector<DuckLakeSnapshotInfo> GetAllSnapshots();

private:
	template <class T>
	void FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name, const string &id_name,
	               set<T> &dropped_entries);

protected:
	DuckLakeTransaction &transaction;
};

} // namespace duckdb