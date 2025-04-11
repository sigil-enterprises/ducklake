//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/connection.hpp"
#include "storage/ducklake_catalog_set.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "common/ducklake_snapshot.hpp"
#include "common/ducklake_data_file.hpp"
#include "duckdb/common/types/value_map.hpp"

namespace duckdb {
class DuckLakeCatalog;
class DuckLakeCatalogSet;
class DuckLakeMetadataManager;
class DuckLakeSchemaEntry;
class DuckLakeTableEntry;
class DuckLakeViewEntry;
struct DuckLakeTableStats;
struct SnapshotChangeInformation;
struct TransactionChangeInformation;
struct NewTableInfo;

class DuckLakeTransaction : public Transaction {
public:
	DuckLakeTransaction(DuckLakeCatalog &ducklake_catalog, TransactionManager &manager, ClientContext &context);
	~DuckLakeTransaction() override;

public:
	void Start();
	void Commit();
	void Rollback();

	DuckLakeCatalog &GetCatalog() {
		return ducklake_catalog;
	}
	DuckLakeMetadataManager &GetMetadataManager() {
		return *metadata_manager;
	}
	unique_ptr<QueryResult> Query(DuckLakeSnapshot snapshot, string query);
	unique_ptr<QueryResult> Query(string query);
	Connection &GetConnection();

	DuckLakeSnapshot GetSnapshot();
	DuckLakeSnapshot GetSnapshot(optional_ptr<BoundAtClause> at_clause);

	static DuckLakeTransaction &Get(ClientContext &context, Catalog &catalog);

	void CreateEntry(unique_ptr<CatalogEntry> entry);
	void DropEntry(CatalogEntry &entry);
	bool IsDeleted(CatalogEntry &entry);

	void AlterEntry(CatalogEntry &old_entry, unique_ptr<CatalogEntry> new_entry);

	DuckLakeCatalogSet &GetOrCreateTransactionLocalEntries(CatalogEntry &entry);
	optional_ptr<DuckLakeCatalogSet> GetTransactionLocalSchemas();
	optional_ptr<DuckLakeCatalogSet> GetTransactionLocalEntries(CatalogType type, const string &schema_name);
	optional_ptr<CatalogEntry> GetTransactionLocalEntry(CatalogType catalog_type, const string &schema_name,
	                                                    const string &entry_name);
	vector<DuckLakeDataFile> GetTransactionLocalFiles(TableIndex table_id);
	void DropTransactionLocalFile(TableIndex table_id, const string &path);
	bool HasTransactionLocalChanges(TableIndex table_id);
	void AppendFiles(TableIndex table_id, vector<DuckLakeDataFile> files);
	void AddDeletes(TableIndex table_id, vector<DuckLakeDeleteFile> files);

	void DropSchema(DuckLakeSchemaEntry &schema);
	void DropTable(DuckLakeTableEntry &table);
	void DropView(DuckLakeViewEntry &view);
	void DropFile(TableIndex table_id, DataFileIndex data_file_id, string path);

	bool SchemaChangesMade();
	bool ChangesMade();
	idx_t GetLocalCatalogId();
	static bool IsTransactionLocal(idx_t id) {
		return id >= DuckLakeConstants::TRANSACTION_LOCAL_ID_START;
	}

	string GetDefaultSchemaName();

	bool HasLocalDeletes(TableIndex table_id);
	void GetLocalDeleteForFile(TableIndex table_id, const string &path, string &delete_path,
	                           string &delete_encryption_key);
	void TransactionLocalDelete(TableIndex table_id, const string &data_path, DuckLakeDeleteFile delete_file);

	bool HasDroppedFiles() const;
	bool FileIsDropped(const string &path) const;

private:
	void CleanupFiles();
	void FlushChanges();
	void FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name, const string &id_name,
	               unordered_set<idx_t> &dropped_entries);
	vector<DuckLakeSchemaInfo> GetNewSchemas(DuckLakeSnapshot &commit_snapshot);
	NewTableInfo GetNewTables(DuckLakeSnapshot &commit_snapshot, TransactionChangeInformation &transaction_changes);
	DuckLakePartitionInfo GetNewPartitionKey(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &tabletable_id);
	DuckLakeTableInfo GetNewTable(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table);
	DuckLakeViewInfo GetNewView(DuckLakeSnapshot &commit_snapshot, DuckLakeViewEntry &view);
	void FlushNewPartitionKey(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table);
	vector<DuckLakeFileInfo> GetNewDataFiles(DuckLakeSnapshot &commit_snapshot);
	vector<DuckLakeDeleteFileInfo> GetNewDeleteFiles(DuckLakeSnapshot &commit_snapshot,
	                                                 set<DataFileIndex> &overwritten_delete_files);
	void UpdateGlobalTableStats(TableIndex table_id, DuckLakeTableStats new_stats);
	void CheckForConflicts(DuckLakeSnapshot transaction_snapshot, const TransactionChangeInformation &changes);
	void CheckForConflicts(const TransactionChangeInformation &changes, const SnapshotChangeInformation &other_changes);
	void WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot, TransactionChangeInformation &changes);
	//! Return the set of changes made by this transaction
	TransactionChangeInformation GetTransactionChanges();
	void GetNewTableInfo(DuckLakeSnapshot &commit_snapshot, reference<CatalogEntry> table_entry, NewTableInfo &result,
	                     TransactionChangeInformation &transaction_changes);
	void GetNewViewInfo(DuckLakeSnapshot &commit_snapshot, reference<CatalogEntry> table_entry, NewTableInfo &result,
	                    TransactionChangeInformation &transaction_changes);

	void AlterEntryInternal(DuckLakeTableEntry &old_entry, unique_ptr<CatalogEntry> new_entry);
	void AlterEntryInternal(DuckLakeViewEntry &old_entry, unique_ptr<CatalogEntry> new_entry);

private:
	DuckLakeCatalog &ducklake_catalog;
	DatabaseInstance &db;
	unique_ptr<DuckLakeMetadataManager> metadata_manager;
	unique_ptr<Connection> connection;
	//! The snapshot of the transaction (latest snapshot in DuckLake)
	unique_ptr<DuckLakeSnapshot> snapshot;
	idx_t local_catalog_id;
	//! New tables added by this transaction
	case_insensitive_map_t<unique_ptr<DuckLakeCatalogSet>> new_tables;
	set<TableIndex> dropped_tables;
	set<TableIndex> dropped_views;
	unordered_map<string, DataFileIndex> dropped_files;
	set<TableIndex> tables_deleted_from;
	//! Schemas added by this transaction
	unique_ptr<DuckLakeCatalogSet> new_schemas;
	map<SchemaIndex, reference<DuckLakeSchemaEntry>> dropped_schemas;
	//! Data files added by this transaction
	map<TableIndex, vector<DuckLakeDataFile>> new_data_files;
	//! New deletes added by this transaction
	map<TableIndex, unordered_map<string, DuckLakeDeleteFile>> new_delete_files;
	//! Snapshot cache for the AT (...) conditions that are referenced in the transaction
	value_map_t<DuckLakeSnapshot> snapshot_cache;
};

} // namespace duckdb
