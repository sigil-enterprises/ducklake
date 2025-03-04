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

namespace duckdb {
class DuckLakeCatalog;

class DuckLakeTransaction : public Transaction {
public:
	DuckLakeTransaction(DuckLakeCatalog &ducklake_catalog, TransactionManager &manager, ClientContext &context);
	~DuckLakeTransaction() override;

	void Start();
	void Commit();
	void Rollback();

	static DuckLakeTransaction &Get(ClientContext &context, Catalog &catalog);

private:
	DuckLakeCatalog &ducklake_catalog;
	optional_idx snapshot_id;
};

} // namespace duckdb
