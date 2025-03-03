#include "ducklake_transaction_manager.hpp"

namespace duckdb {

DuckLakeTransactionManager::DuckLakeTransactionManager(AttachedDatabase &db_p, DuckLakeCatalog &ducklake_catalog)
	: TransactionManager(db_p), ducklake_catalog(ducklake_catalog) {
}

Transaction &DuckLakeTransactionManager::StartTransaction(ClientContext &context) {
	throw InternalException("Unsupported DuckLake TM function");
}

ErrorData DuckLakeTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	throw InternalException("Unsupported DuckLake TM function");
}

void DuckLakeTransactionManager::RollbackTransaction(Transaction &transaction) {
	throw InternalException("Unsupported DuckLake TM function");
}

void DuckLakeTransactionManager::Checkpoint(ClientContext &context, bool force) {
	throw InternalException("Unsupported DuckLake TM function");
}

}
