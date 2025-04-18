#include "storage/ducklake_transaction_manager.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"

namespace duckdb {

void DuckLakeTransactionManager::Checkpoint(ClientContext &context, bool force) {
	throw NotImplementedException("CHECKPOINT not supported for DuckLake yet");
}

} // namespace duckdb
