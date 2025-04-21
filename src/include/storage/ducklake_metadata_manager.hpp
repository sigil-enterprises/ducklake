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
#include "storage/ducklake_metadata_info.hpp"
#include "common/ducklake_encryption.hpp"
#include "common/index.hpp"

namespace duckdb {
class DuckLakeCatalogSet;
class DuckLakeSchemaEntry;
class DuckLakeTableEntry;
class DuckLakeTransaction;
class BoundAtClause;

// The DuckLake metadata manger is the communication layer between the system and the metadata catalog
class DuckLakeMetadataManager {
public:
	DuckLakeMetadataManager(DuckLakeTransaction &transaction);
	virtual ~DuckLakeMetadataManager();

	DuckLakeMetadataManager &Get(DuckLakeTransaction &transaction);

	//! Initialize a new DuckLake
	virtual void InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption);
	virtual DuckLakeMetadata LoadDuckLake();
	//! Get the catalog information for a specific snapshot
	virtual DuckLakeCatalogInfo GetCatalogForSnapshot(DuckLakeSnapshot snapshot);
	virtual vector<DuckLakeGlobalStatsInfo> GetGlobalTableStats(DuckLakeSnapshot snapshot);
	virtual vector<DuckLakeFileListEntry> GetFilesForTable(DuckLakeSnapshot snapshot, TableIndex table_id,
	                                                       const string &filter);
	virtual vector<DuckLakeFileListEntry> GetTableInsertions(DuckLakeSnapshot start_snapshot, DuckLakeSnapshot snapshot,
	                                                         TableIndex table_id);
	virtual vector<DuckLakeDeleteScanEntry> GetTableDeletions(DuckLakeSnapshot start_snapshot,
	                                                          DuckLakeSnapshot snapshot, TableIndex table_id);
	virtual vector<DuckLakeFileListExtendedEntry> GetExtendedFilesForTable(DuckLakeSnapshot snapshot,
	                                                                       TableIndex table_id, const string &filter);
	virtual vector<DuckLakeCompactionFileEntry> GetFilesForCompaction(TableIndex table_id);
	virtual vector<DuckLakeFileScheduledForCleanup> GetFilesScheduledForCleanup(const string &filter);
	virtual void RemoveFilesScheduledForCleanup(const vector<DuckLakeFileScheduledForCleanup> &cleaned_up_files);
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
	virtual void DropDataFiles(DuckLakeSnapshot commit_snapshot, const set<DataFileIndex> &dropped_files);
	virtual void DropDeleteFiles(DuckLakeSnapshot commit_snapshot, const set<DataFileIndex> &dropped_files);
	virtual void WriteNewDeleteFiles(DuckLakeSnapshot commit_snapshot,
	                                 const vector<DuckLakeDeleteFileInfo> &new_delete_files);
	virtual void WriteCompactions(vector<DuckLakeCompactedFileInfo> compactions);
	virtual void InsertSnapshot(DuckLakeSnapshot commit_snapshot);
	virtual void WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot, const SnapshotChangeInfo &change_info);
	virtual void UpdateGlobalTableStats(const DuckLakeGlobalStatsInfo &stats);
	virtual SnapshotChangeInfo GetChangesMadeAfterSnapshot(DuckLakeSnapshot start_snapshot);
	virtual unique_ptr<DuckLakeSnapshot> GetSnapshot();
	virtual unique_ptr<DuckLakeSnapshot> GetSnapshot(BoundAtClause &at_clause);
	virtual idx_t GetNextColumnId(TableIndex table_id);

	virtual vector<DuckLakeSnapshotInfo> GetAllSnapshots();

private:
	template <class T>
	void FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name, const string &id_name,
	               const set<T> &dropped_entries);

	bool IsEncrypted() const;
	string GetFileSelectList(const string &prefix);

protected:
	DuckLakeTransaction &transaction;
};

} // namespace duckdb