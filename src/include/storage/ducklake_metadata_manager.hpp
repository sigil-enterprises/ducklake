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
#include "common/ducklake_snapshot.hpp"

namespace duckdb {
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

// The DuckLake metadata manger is the communication layer between the system and the metadata catalog
class DuckLakeMetadataManager {
public:
	DuckLakeMetadataManager(DuckLakeTransaction &transaction);
	virtual ~DuckLakeMetadataManager();

	DuckLakeMetadataManager &Get(DuckLakeTransaction &transaction);

	virtual void DropSchemas(DuckLakeSnapshot commit_snapshot, unordered_set<idx_t> ids);
	virtual void DropTables(DuckLakeSnapshot commit_snapshot, unordered_set<idx_t> ids);
	virtual void WriteNewSchemas(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeSchemaInfo> &new_schemas);
	virtual void WriteNewTables(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeTableInfo> &new_tables);
	virtual void WriteNewDataFiles(DuckLakeSnapshot commit_snapshot, const vector<DuckLakeFileInfo> &new_files);

private:
	void FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name, const string &id_name,
	               unordered_set<idx_t> &dropped_entries);

protected:
	DuckLakeTransaction &transaction;
};

} // namespace duckdb