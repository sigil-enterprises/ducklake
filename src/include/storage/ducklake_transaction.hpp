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

namespace duckdb {
class DuckLakeCatalog;
class DuckLakeCatalogSet;
class DuckLakeMetadataManager;
class DuckLakeSchemaEntry;
class DuckLakeTableEntry;
struct DuckLakeTableStats;
struct SnapshotChangeInformation;
struct TransactionChangeInformation;

class DuckLakeTransaction : public Transaction {
public:
	DuckLakeTransaction(DuckLakeCatalog &ducklake_catalog, TransactionManager &manager, ClientContext &context);
	~DuckLakeTransaction() override;

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
	void AppendFiles(TableIndex table_id, const vector<DuckLakeDataFile> &files);

	void DropSchema(DuckLakeSchemaEntry &schema);
	void DropTable(DuckLakeTableEntry &table);

	bool SchemaChangesMade();
	bool ChangesMade();
	idx_t GetLocalCatalogId();
	static bool IsTransactionLocal(idx_t id) {
		return id >= DuckLakeConstants::TRANSACTION_LOCAL_ID_START;
	}

	string GetDefaultSchemaName();

private:
	void CleanupFiles();
	void FlushChanges();
	void FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name, const string &id_name,
	               unordered_set<idx_t> &dropped_entries);
	vector<DuckLakeSchemaInfo> GetNewSchemas(DuckLakeSnapshot &commit_snapshot);
	vector<DuckLakeTableInfo> GetNewTables(DuckLakeSnapshot &commit_snapshot,
	                                       vector<DuckLakePartitionInfo> &new_partition_keys);
	DuckLakePartitionInfo GetNewPartitionKey(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table,
	                                         TableIndex table_id);
	DuckLakeTableInfo GetNewTable(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table);
	void FlushNewPartitionKey(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table);
	vector<DuckLakeFileInfo> GetNewDataFiles(DuckLakeSnapshot &commit_snapshot);
	void UpdateGlobalTableStats(TableIndex table_id, DuckLakeTableStats new_stats);
	void CheckForConflicts(DuckLakeSnapshot transaction_snapshot, const TransactionChangeInformation &changes);
	void CheckForConflicts(const TransactionChangeInformation &changes, const SnapshotChangeInformation &other_changes);
	void WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot, TransactionChangeInformation &changes);
	//! Return the set of changes made by this transaction
	TransactionChangeInformation GetTransactionChanges();

private:
	DuckLakeCatalog &ducklake_catalog;
	DatabaseInstance &db;
	unique_ptr<DuckLakeMetadataManager> metadata_manager;
	unique_ptr<Connection> connection;
	unique_ptr<DuckLakeSnapshot> snapshot;
	idx_t local_catalog_id;
	//! New tables added by this transaction
	case_insensitive_map_t<unique_ptr<DuckLakeCatalogSet>> new_tables;
	set<TableIndex> dropped_tables;
	//! Schemas added by this transaction
	unique_ptr<DuckLakeCatalogSet> new_schemas;
	map<SchemaIndex, reference<DuckLakeSchemaEntry>> dropped_schemas;
	//! Data files added by this transaction
	map<TableIndex, vector<DuckLakeDataFile>> new_data_files;
};

} // namespace duckdb
