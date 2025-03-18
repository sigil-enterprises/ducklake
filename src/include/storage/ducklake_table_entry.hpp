//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_table_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "storage/ducklake_stats.hpp"
#include "storage/ducklake_partition_data.hpp"
#include "common/index.hpp"

namespace duckdb {
struct AlterTableInfo;
struct SetPartitionedByInfo;
class DuckLakeTransaction;

enum class TransactionLocalChange { NONE, CREATED, RENAMED, SET_PARTITION_KEY };

struct DuckLakeFieldId {
	FieldIndex id;
	vector<DuckLakeFieldId> children;
};

class DuckLakeTableEntry : public TableCatalogEntry {
public:
	DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, TableIndex table_id,
	                   string table_uuid, vector<DuckLakeFieldId> field_ids, TransactionLocalChange transaction_local_change);

public:
	TableIndex GetTableId() const {
		return table_id;
	}
	const string &GetTableUUID() const {
		return table_uuid;
	}
	bool IsTransactionLocal() const {
		return transaction_local_change != TransactionLocalChange::NONE;
	}
	TransactionLocalChange LocalChange() const {
		return transaction_local_change;
	}
	optional_ptr<DuckLakePartition> GetPartitionData() {
		return partition_data.get();
	}
	const vector<DuckLakeFieldId> &GetFieldIds() const {
		return field_ids;
	}
	void SetPartitionData(unique_ptr<DuckLakePartition> partition_data);
	optional_ptr<DuckLakeTableStats> GetTableStats(ClientContext &context);

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	unique_ptr<CatalogEntry> Alter(DuckLakeTransaction &transaction, AlterTableInfo &info);

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

private:
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, RenameTableInfo &info);
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, SetPartitionedByInfo &info);

public:
	// ! Create a DuckLakeTableEntry from a RENAME
	DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info);
	// ! Create a DuckLakeTableEntry from a SET PARTITION KEY
	DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, unique_ptr<DuckLakePartition> partition_data);

private:
	TableIndex table_id;
	string table_uuid;
	vector<DuckLakeFieldId> field_ids;
	TransactionLocalChange transaction_local_change;
	unique_ptr<DuckLakePartition> partition_data;
};

} // namespace duckdb
