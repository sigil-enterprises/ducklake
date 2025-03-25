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
#include "storage/ducklake_field_data.hpp"
#include "common/local_change.hpp"

namespace duckdb {
struct AlterTableInfo;
struct DuckLakeColumnInfo;
struct SetPartitionedByInfo;
struct SetCommentInfo;
class DuckLakeTransaction;

class DuckLakeTableEntry : public TableCatalogEntry {
public:
	DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, TableIndex table_id,
	                   string table_uuid, shared_ptr<DuckLakeFieldData> field_data, FieldIndex next_column_id,
	                   LocalChange local_change);

public:
	TableIndex GetTableId() const {
		return table_id;
	}
	const string &GetTableUUID() const {
		return table_uuid;
	}
	void SetTableId(TableIndex new_table_id) {
		table_id = new_table_id;
	}
	bool IsTransactionLocal() const {
		return local_change.type != LocalChangeType::NONE;
	}
	LocalChange GetLocalChange() const {
		return local_change;
	}
	optional_ptr<DuckLakePartition> GetPartitionData() {
		return partition_data.get();
	}
	DuckLakeFieldData &GetFieldData() {
		return *field_data;
	}
	FieldIndex GetNextColumnId() const {
		return next_column_id;
	}
	const ColumnDefinition &GetColumnByFieldId(FieldIndex field_index) const;
	//! Returns the root field id of a column
	const DuckLakeFieldId &GetFieldId(PhysicalIndex column_index) const;
	//! Returns the field id of a column by a column path
	const DuckLakeFieldId &GetFieldId(const vector<string> &column_names) const;
	//! Returns the field id of a column by a field index
	const DuckLakeFieldId &GetFieldId(FieldIndex field_index) const;
	void SetPartitionData(unique_ptr<DuckLakePartition> partition_data);
	optional_ptr<DuckLakeTableStats> GetTableStats(ClientContext &context);
	optional_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction);

	//! Gets the top-level not-null fields
	case_insensitive_set_t GetNotNullFields() const;

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                              const EntryLookupInfo &lookup_info) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	unique_ptr<CatalogEntry> Alter(DuckLakeTransaction &transaction, AlterTableInfo &info);
	unique_ptr<CatalogEntry> Alter(DuckLakeTransaction &transaction, SetCommentInfo &info);
	unique_ptr<CatalogEntry> Alter(DuckLakeTransaction &transaction, SetColumnCommentInfo &info);

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	DuckLakeColumnInfo GetColumnInfo(FieldIndex field_index) const;
	DuckLakeColumnInfo GetAddColumnInfo() const;

	static DuckLakeColumnInfo ConvertColumn(const string &name, const LogicalType &type,
	                                        const DuckLakeFieldId &field_id);

private:
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, RenameTableInfo &info);
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, SetPartitionedByInfo &info);
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, SetNotNullInfo &info);
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, DropNotNullInfo &info);
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, RenameColumnInfo &info);
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, AddColumnInfo &info);
	unique_ptr<CatalogEntry> AlterTable(DuckLakeTransaction &transaction, RemoveColumnInfo &info);

public:
	// ! Create a DuckLakeTableEntry from an ALTER
	DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, LocalChange local_change);
	// ! Create a DuckLakeTableEntry from a RENAME COLUMN
	DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, LocalChange local_change,
	                   const string &new_name);
	// ! Create a DuckLakeTableEntry from a SET PARTITION KEY
	DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, unique_ptr<DuckLakePartition> partition_data);

private:
	TableIndex table_id;
	string table_uuid;
	shared_ptr<DuckLakeFieldData> field_data;
	FieldIndex next_column_id;
	LocalChange local_change;
	unique_ptr<DuckLakePartition> partition_data;
};

} // namespace duckdb
