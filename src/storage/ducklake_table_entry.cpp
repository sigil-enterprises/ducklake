#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_transaction.hpp"

#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"

namespace duckdb {

DuckLakeTableEntry::DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       TableIndex table_id, string table_uuid_p,
                                       shared_ptr<DuckLakeFieldData> field_data_p,
                                       TransactionLocalChange transaction_local_change)
    : TableCatalogEntry(catalog, schema, info), table_id(table_id), table_uuid(std::move(table_uuid_p)),
      field_data(std::move(field_data_p)), transaction_local_change(transaction_local_change) {
	D_ASSERT(field_data->GetColumnCount() == GetColumns().PhysicalColumnCount());
}

// ALTER TABLE RENAME
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info)
    : DuckLakeTableEntry(parent.ParentCatalog(), parent.ParentSchema(), info, parent.GetTableId(),
                         parent.GetTableUUID(), parent.field_data, TransactionLocalChange::RENAMED) {
	if (parent.partition_data) {
		partition_data = make_uniq<DuckLakePartition>(*parent.partition_data);
	}
}

// ALTER TABLE SET PARTITION KEY
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info,
                                       unique_ptr<DuckLakePartition> partition_data_p)
    : DuckLakeTableEntry(parent.ParentCatalog(), parent.ParentSchema(), info, parent.GetTableId(),
                         parent.GetTableUUID(), parent.field_data, TransactionLocalChange::SET_PARTITION_KEY) {
	partition_data = std::move(partition_data_p);
}

const DuckLakeFieldId &DuckLakeTableEntry::GetFieldId(PhysicalIndex column_index) const {
	return field_data->GetByRootIndex(column_index);
}

const DuckLakeFieldId &DuckLakeTableEntry::GetFieldId(FieldIndex field_index) const {
	return field_data->GetByFieldIndex(field_index);
}

const DuckLakeFieldId &DuckLakeTableEntry::GetFieldId(const vector<string> &column_names) const {
	auto &root_col = columns.GetColumn(column_names[0]);
	return field_data->GetByNames(root_col.Physical(), column_names);
}

unique_ptr<BaseStatistics> GetColumnStats(const DuckLakeFieldId &field_id, const DuckLakeTableStats &table_stats) {
	auto &field_children = field_id.Children();
	if (field_children.empty()) {
		// non-nested type - lookup the field id in the stats map
		auto entry = table_stats.column_stats.find(field_id.GetFieldIndex());
		if (entry == table_stats.column_stats.end()) {
			return nullptr;
		}
		return entry->second.ToStats();
	}
	// nested type
	switch (field_id.Type().id()) {
	case LogicalTypeId::STRUCT: {
		auto struct_stats = StructStats::CreateUnknown(field_id.Type());
		for (idx_t child_idx = 0; child_idx < field_children.size(); ++child_idx) {
			auto child_stats = GetColumnStats(*field_children[child_idx], table_stats);
			StructStats::SetChildStats(struct_stats, child_idx, std::move(child_stats));
		}
		return struct_stats.ToUnique();
	}
	case LogicalTypeId::LIST: {
		auto list_stats = ListStats::CreateUnknown(field_id.Type());
		auto child_stats = GetColumnStats(*field_children[0], table_stats);
		ListStats::SetChildStats(list_stats, std::move(child_stats));
		return list_stats.ToUnique();
	}
	default:
		// unsupported nested type
		return nullptr;
	}
}

unique_ptr<BaseStatistics> DuckLakeTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	auto table_stats = GetTableStats(context);
	if (!table_stats) {
		return nullptr;
	}
	auto &field_id = field_data->GetByRootIndex(PhysicalIndex(column_id));
	return GetColumnStats(field_id, *table_stats);
}

TableFunction DuckLakeTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	throw InternalException("DuckLakeTableEntry::GetScanFunction called without entry lookup info");
}

TableFunction DuckLakeTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
                                                  const EntryLookupInfo &lookup_info) {
	auto function = DuckLakeFunctions::GetDuckLakeScanFunction(*context.db);
	auto &transaction = DuckLakeTransaction::Get(context, ParentCatalog());

	auto function_info =
	    make_shared_ptr<DuckLakeFunctionInfo>(*this, transaction.GetSnapshot(lookup_info.GetAtClause()));
	function_info->table_name = name;
	for (auto &col : columns.Logical()) {
		function_info->column_names.push_back(col.Name());
		function_info->column_types.push_back(col.Type());
	}
	function_info->table_id = GetTableId();
	function.function_info = std::move(function_info);

	vector<Value> inputs {Value("")};
	named_parameter_map_t param_map;
	vector<LogicalType> return_types;
	vector<string> names;
	TableFunctionRef empty_ref;

	TableFunctionBindInput bind_input(inputs, param_map, return_types, names, nullptr, nullptr, function, empty_ref);

	auto result = function.bind(context, bind_input, return_types, names);
	bind_data = std::move(result);

	return function;
}

void DuckLakeTableEntry::SetPartitionData(unique_ptr<DuckLakePartition> partition_data_p) {
	partition_data = std::move(partition_data_p);
}

optional_ptr<DuckLakeTableStats> DuckLakeTableEntry::GetTableStats(ClientContext &context) {
	if (IsTransactionLocal()) {
		// no stats for transaction local tables
		return nullptr;
	}
	auto &dl_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ParentCatalog());
	return dl_catalog.GetTableStats(transaction, GetTableId());
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, RenameTableInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	table_info.table = info.new_table_name;
	// create a complete copy of this table with only the name changed
	return make_uniq<DuckLakeTableEntry>(*this, table_info);
}

DuckLakePartitionField GetPartitionField(DuckLakeTableEntry &table, ParsedExpression &expr) {
	if (expr.type != ExpressionType::COLUMN_REF) {
		throw NotImplementedException("Unsupported partition key %s - only columns are supported", expr.ToString());
	}
	auto &colref = expr.Cast<ColumnRefExpression>();
	if (colref.IsQualified()) {
		throw InvalidInputException("Unexpected qualified column reference - only columns are supported");
	}
	DuckLakePartitionField field;
	auto &col = table.GetColumn(colref.GetColumnName());
	field.column_id = col.Oid();
	field.transform.type = DuckLakeTransformType::IDENTITY;
	return field;
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, SetPartitionedByInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	// create a complete copy of this table with the partition info added
	auto partition_data = make_uniq<DuckLakePartition>();
	partition_data->partition_id = transaction.GetLocalCatalogId();
	for (idx_t expr_idx = 0; expr_idx < info.partition_keys.size(); expr_idx++) {
		auto &expr = *info.partition_keys[expr_idx];
		auto partition_field = GetPartitionField(*this, expr);
		partition_field.partition_key_index = expr_idx;
		partition_data->fields.push_back(partition_field);
	}

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info, std::move(partition_data));
	return std::move(new_entry);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::Alter(DuckLakeTransaction &transaction, AlterTableInfo &info) {
	switch (info.alter_table_type) {
	case AlterTableType::RENAME_TABLE:
		return AlterTable(transaction, info.Cast<RenameTableInfo>());
	case AlterTableType::SET_PARTITIONED_BY:
		return AlterTable(transaction, info.Cast<SetPartitionedByInfo>());
	default:
		throw BinderException("Unsupported ALTER TABLE type - DuckLake tables only support RENAME TABLE for now");
	}
}

TableStorageInfo DuckLakeTableEntry::GetStorageInfo(ClientContext &context) {
	return TableStorageInfo();
}

void DuckLakeTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                               LogicalUpdate &update, ClientContext &context) {
	throw InternalException("Unsupported function BindUpdateConstraints for table entry");
}

} // namespace duckdb
