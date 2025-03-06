//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_table_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {
struct AlterTableInfo;
class DuckLakeTransaction;

class DuckLakeTableEntry : public TableCatalogEntry {
public:
	DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, idx_t table_id,
	                   string table_uuid, bool is_transaction_local);

public:
	idx_t GetTableId() const {
		return table_id;
	}
	const string &GetTableUUID() const {
		return table_uuid;
	}
	bool IsTransactionLocal() const {
		return is_transaction_local;
	}

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	unique_ptr<CatalogEntry> Alter(DuckLakeTransaction &transaction, AlterTableInfo &info);

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

private:
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, RenameTableInfo &info);

private:
	idx_t table_id;
	string table_uuid;
	bool is_transaction_local;
};

} // namespace duckdb
