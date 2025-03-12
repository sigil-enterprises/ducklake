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

namespace duckdb {

DuckLakeTableEntry::DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       idx_t table_id, string table_uuid_p,
                                       TransactionLocalChange transaction_local_change)
    : TableCatalogEntry(catalog, schema, info), table_id(table_id), table_uuid(std::move(table_uuid_p)),
      transaction_local_change(transaction_local_change) {
}

// ALTER TABLE RENAME
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info)
    : DuckLakeTableEntry(parent.ParentCatalog(), parent.ParentSchema(), info, parent.GetTableId(),
                         parent.GetTableUUID(), TransactionLocalChange::RENAMED) {
	if (parent.partition_data) {
		partition_data = make_uniq<DuckLakePartition>(*parent.partition_data);
	}
}

// ALTER TABLE SET PARTITION KEY
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info,
                                       unique_ptr<DuckLakePartition> partition_data_p)
    : DuckLakeTableEntry(parent.ParentCatalog(), parent.ParentSchema(), info, parent.GetTableId(),
                         parent.GetTableUUID(), TransactionLocalChange::SET_PARTITION_KEY) {
	partition_data = std::move(partition_data_p);
}

unique_ptr<BaseStatistics> DuckLakeTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction DuckLakeTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto function = DuckLakeFunctions::GetDuckLakeScanFunction(*context.db);
	auto &transaction = DuckLakeTransaction::Get(context, ParentCatalog());

	auto function_info = make_shared_ptr<DuckLakeFunctionInfo>(*this, transaction.GetSnapshot());
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
