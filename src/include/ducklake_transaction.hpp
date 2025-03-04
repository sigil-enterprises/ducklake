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

namespace duckdb {
class DuckLakeCatalog;

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

private:
	DuckLakeCatalog &ducklake_catalog;
	DatabaseInstance &db;
	unique_ptr<Connection> connection;
	unique_ptr<DuckLakeSnapshot> snapshot;
};

} // namespace duckdb
