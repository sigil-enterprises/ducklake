//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storageducklake_transaction_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction_manager.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"

namespace duckdb {

class DuckLakeTransactionManager : public TransactionManager {
public:
	DuckLakeTransactionManager(AttachedDatabase &db_p, DuckLakeCatalog &ducklake_catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;

	void Checkpoint(ClientContext &context, bool force = false) override;

	static constexpr const char *DUCKLAKE_CHECKPOINT_QUERY = R"(
    WITH flush AS (SELECT * FROM ducklake_flush_inlined_data({CATALOG})),
         expire AS (SELECT * FROM ducklake_expire_snapshots({CATALOG})),
         merge AS (SELECT * FROM ducklake_merge_adjacent_files({CATALOG})),
         rewrite AS (SELECT * FROM ducklake_rewrite_data_files({CATALOG})),
         cleanup AS (SELECT * FROM ducklake_cleanup_old_files({CATALOG})),
         orphans AS (SELECT * FROM ducklake_delete_orphaned_files({CATALOG}))
    SELECT #1 FROM flush
    UNION ALL SELECT #1 FROM expire
    UNION ALL SELECT #1 FROM merge
    UNION ALL SELECT #1 FROM rewrite
    UNION ALL SELECT #1 FROM cleanup
    UNION ALL SELECT #1 FROM orphans;
)";

private:
	DuckLakeCatalog &ducklake_catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, shared_ptr<DuckLakeTransaction>> transactions;
	bool get_snapshot = true;
};

} // namespace duckdb
