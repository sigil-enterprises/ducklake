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

namespace duckdb {
class DuckLakeCatalog;
class DuckLakeCatalogSet;

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
	unique_ptr<QueryResult> Query(DuckLakeSnapshot snapshot, string query);
	unique_ptr<QueryResult> Query(string query);
	Connection &GetConnection();

	DuckLakeSnapshot GetSnapshot();

	static DuckLakeTransaction &Get(ClientContext &context, Catalog &catalog);

	DuckLakeCatalogSet &GetOrCreateNewTableElements(const string &schema_name);
	optional_ptr<DuckLakeCatalogSet> GetNewTableElements(const string &schema_name);
	void AppendFiles(idx_t table_id, const vector<string> &files);

	bool ChangesMade();
	void FlushChanges();
	idx_t GetLocalTableId();

private:
	DuckLakeCatalog &ducklake_catalog;
	DatabaseInstance &db;
	unique_ptr<Connection> connection;
	unique_ptr<DuckLakeSnapshot> snapshot;
	idx_t local_table_id;
	//! New tables added by this transaction
	case_insensitive_map_t<unique_ptr<DuckLakeCatalogSet>> new_tables;
	unordered_map<idx_t, vector<string>> new_data_files;
};

} // namespace duckdb
