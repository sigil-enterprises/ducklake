//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storageducklake_transaction_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

class DuckLakeTransactionManager : public TransactionManager {
public:
	DuckLakeTransactionManager(AttachedDatabase &db_p, DuckLakeCatalog &ducklake_catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;

	void Checkpoint(ClientContext &context, bool force = false) override;

	//! Returns the current version of the catalog:
	//! If there are no uncommitted changes, this is the schema version of the snapshot.
	//! Otherwise, an id that is incremented whenever the schema changes (not stored between restarts)
	idx_t GetCatalogVersion(Transaction &transaction);

private:
	DuckLakeCatalog &ducklake_catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, shared_ptr<DuckLakeTransaction>> transactions;

	atomic<idx_t> last_uncommitted_catalog_version = {TRANSACTION_ID_START};

	friend DuckLakeTransaction;
};

} // namespace duckdb
