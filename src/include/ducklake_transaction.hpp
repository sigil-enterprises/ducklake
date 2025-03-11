//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/connection.hpp"
#include "ducklake_snapshot.hpp"
#include "ducklake_catalog_set.hpp"
#include "ducklake_data_file.hpp"

namespace duckdb {
class DuckLakeCatalog;
class DuckLakeCatalogSet;
class DuckLakeSchemaEntry;
class DuckLakeTableEntry;
struct SnapshotChangeInformation;
struct TransactionChangeInformation;

class DuckLakeTransaction : public Transaction {
public:
	static constexpr const idx_t TRANSACTION_LOCAL_ID_START = 9223372036854775808ULL;

public:
	DuckLakeTransaction(DuckLakeCatalog &ducklake_catalog, TransactionManager &manager, ClientContext &context);
	~DuckLakeTransaction() override;

	void Start();
	void Commit();
	void Rollback();

	DuckLakeCatalog &GetCatalog() {
		return ducklake_catalog;
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
	vector<DuckLakeDataFile> GetTransactionLocalFiles(idx_t table_id);
	void AppendFiles(idx_t table_id, const vector<DuckLakeDataFile> &files);

	void DropSchema(DuckLakeSchemaEntry &schema);
	void DropTable(DuckLakeTableEntry &table);

	bool SchemaChangesMade();
	bool ChangesMade();
	idx_t GetLocalCatalogId();
	static bool IsTransactionLocal(idx_t id) {
		return id >= TRANSACTION_LOCAL_ID_START;
	}

	string GetDefaultSchemaName();

private:
	void CleanupFiles();
	void FlushChanges();
	void FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name, const string &id_name,
	               unordered_set<idx_t> &dropped_entries);
	void CheckForConflicts(DuckLakeSnapshot transaction_snapshot, const TransactionChangeInformation &changes);
	void CheckForConflicts(const TransactionChangeInformation &changes, const SnapshotChangeInformation &other_changes);
	void WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot, const TransactionChangeInformation &changes);
	//! Return the set of changes made by this transaction
	TransactionChangeInformation GetTransactionChanges();

private:
	DuckLakeCatalog &ducklake_catalog;
	DatabaseInstance &db;
	unique_ptr<Connection> connection;
	unique_ptr<DuckLakeSnapshot> snapshot;
	idx_t local_catalog_id;
	//! New tables added by this transaction
	case_insensitive_map_t<unique_ptr<DuckLakeCatalogSet>> new_tables;
	unordered_set<idx_t> dropped_tables;
	//! Schemas added by this transaction
	unique_ptr<DuckLakeCatalogSet> new_schemas;
	unordered_map<idx_t, reference<DuckLakeSchemaEntry>> dropped_schemas;
	//! Data files added by this transaction
	unordered_map<idx_t, vector<DuckLakeDataFile>> new_data_files;
};

} // namespace duckdb
