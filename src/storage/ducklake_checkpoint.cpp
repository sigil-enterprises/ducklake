#include "ducklake_extension.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/ducklake_transaction_manager.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

void DuckLakeTransactionManager::Checkpoint(ClientContext &context, bool force) {
	if (!context.transaction.IsAutoCommit()) {
		throw InvalidConfigurationException("CHECKPOINT; is only allowed with auto_commit = true");
	}
	// We start a new connection
	auto conn = make_uniq<Connection>(ducklake_catalog.GetDatabase());
	auto checkpoint_query = StringUtil::Replace(DUCKLAKE_CHECKPOINT_QUERY, "{CATALOG}",
	                                            KeywordHelper::WriteQuoted(ducklake_catalog.GetName(), '\''));
	auto res = conn->Query(checkpoint_query);
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to perform CHECKPOINT; in DuckLake:  ");
	}
}

} // namespace duckdb
