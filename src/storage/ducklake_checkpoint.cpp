#include "ducklake_extension.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/ducklake_transaction_manager.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

void DuckLakeTransactionManager::Checkpoint(ClientContext &context, bool force) {
	auto &crazy = DuckLakeTransaction::Get(context,ducklake_catalog);
	if (crazy.ChangesMade()) {
		throw InternalException("bla");
	}
	// auto &conn = crazy.GetConnection();
	// if (context.transaction.ActiveTransaction().) {
	// 	throw InvalidConfigurationException("CHECKPOINT can't be called in a begin transaction.");
	// }
	// ducklake_catalog.db
	// auto &duck_transaction = context.transaction.ActiveTransaction().GetTransaction()
	// 		if (duck_transaction.ChangesMade()) {
	// 			throw TransactionException("Cannot CHECKPOINT: the current transaction has transaction local changes");
	// 		}
	// We start a new connection
	auto conn = make_uniq<Connection>(ducklake_catalog.GetDatabase());
	auto checkpoint_query = StringUtil::Replace(DUCKLAKE_CHECKPOINT_MODIFICATIONS, "{CATALOG}",
	                                            KeywordHelper::WriteQuoted(ducklake_catalog.GetName(), '\''));
	auto res = conn->Query(checkpoint_query);
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to perform CHECKPOINT; in DuckLake:  ");
	}

	checkpoint_query = StringUtil::Replace(DUCKLAKE_CHECKPOINT_COMPACTION, "{CATALOG}",
	                                            KeywordHelper::WriteQuoted(ducklake_catalog.GetName(), '\''));
	res = conn->Query(checkpoint_query);
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to perform CHECKPOINT; in DuckLake:  ");
	}
}

} // namespace duckdb
