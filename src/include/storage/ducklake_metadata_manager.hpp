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
#include "common/ducklake_snapshot.hpp"
#include "storage/ducklake_partition_data.hpp"
#include "storage/ducklake_stats.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {
class DuckLakeCatalogSet;
class DuckLakeSchemaEntry;
class DuckLakeTableEntry;
class DuckLakeTransaction;

struct DuckLakeSchemaInfo {
	idx_t id;
	string uuid;
	string name;
};

struct DuckLakeColumnInfo {
	idx_t id;
	string name;
	string type;
};

struct DuckLakeTableInfo {
	idx_t id;
	idx_t schema_id;
	string uuid;
	string name;
	vector<DuckLakeColumnInfo> columns;
};

struct DuckLakeColumnStatsInfo {
	idx_t column_id;
	string value_count;
	string null_count;
	string min_val;
	string max_val;
};

struct DuckLakeFileInfo {
	idx_t id;
	idx_t table_id;
	string file_name;
	idx_t row_count;
	idx_t file_size_bytes;
	idx_t footer_size;
	vector<DuckLakeColumnStatsInfo> column_stats;
};

struct DuckLakePartitionFieldInfo {
	idx_t partition_key_index = 0;
	idx_t column_id;
	string transform;
};

struct DuckLakePartitionInfo {
	optional_idx id;
	idx_t table_id;
	vector<DuckLakePartitionFieldInfo> fields;
};

struct DuckLakeGlobalColumnStatsInfo {
	idx_t column_id;

	bool contains_null;
	bool has_contains_null;

	string min_val;
	bool has_min;

	string max_val;
	bool has_max;
};

struct DuckLakeGlobalStatsInfo {
	idx_t table_id;
	bool initialized;
	idx_t record_count;
	idx_t table_size_bytes;
	vector<DuckLakeGlobalColumnStatsInfo> column_stats;
};

struct SnapshotChangeInfo {
	string schemas_created;
	string schemas_dropped;
	string tables_created;
	string tables_dropped;
	string tables_altered;
	string tables_inserted_into;
	string tables_deleted_from;
};

struct DuckLakeSnapshotInfo {
	idx_t id;
	timestamp_tz_t time;
	idx_t schema_version;
	SnapshotChangeInfo change_info;
};

struct DuckLakeCatalogInfo {
	vector<DuckLakeSchemaInfo> schemas;
	vector<DuckLakeTableInfo> tables;
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
	//! Get the catalog information for a specific snapshot
	virtual DuckLakeCatalogInfo GetCatalogForSnapshot(DuckLakeSnapshot snapshot);
	virtual vector<DuckLakeGlobalStatsInfo> GetGlobalTableStats(DuckLakeSnapshot snapshot);
	virtual void DropSchemas(DuckLakeSnapshot commit_snapshot, unordered_set<idx_t> ids);
	virtual void DropTables(DuckLakeSnapshot commit_snapshot, unordered_set<idx_t> ids);
	virtual void WriteNewSchemas(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeSchemaInfo> &new_schemas);
	virtual void WriteNewTables(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeTableInfo> &new_tables);
	virtual void WriteNewPartitionKeys(DuckLakeSnapshot commit_snapshot,
	                                   const vector<DuckLakePartitionInfo> &new_partitions);
	virtual void WriteNewDataFiles(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeFileInfo> &new_files);
	virtual void InsertSnapshot(DuckLakeSnapshot commit_snapshot);
	virtual void WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot, const SnapshotChangeInfo &change_info);
	virtual void UpdateGlobalTableStats(const DuckLakeGlobalStatsInfo &stats);
	virtual SnapshotChangeInfo GetChangesMadeAfterSnapshot(DuckLakeSnapshot start_snapshot);
	virtual unique_ptr<DuckLakeSnapshot> GetSnapshot();

	virtual vector<DuckLakeSnapshotInfo> GetAllSnapshots();
private:
	void FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name, const string &id_name,
	               unordered_set<idx_t> &dropped_entries);

protected:
	DuckLakeTransaction &transaction;
};

} // namespace duckdb