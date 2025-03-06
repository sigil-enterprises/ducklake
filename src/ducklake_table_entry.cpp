#include "ducklake_table_entry.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "ducklake_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "ducklake_transaction.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

DuckLakeTableEntry::DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       idx_t table_id, string table_uuid_p, bool is_transaction_local)
    : TableCatalogEntry(catalog, schema, info), table_id(table_id), table_uuid(std::move(table_uuid_p)),
      is_transaction_local(is_transaction_local) {
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

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, RenameTableInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	table_info.table = info.new_table_name;
	// create a complete copy of this table with only the name changed
	return make_uniq<DuckLakeTableEntry>(ParentCatalog(), ParentSchema(), table_info, GetTableId(), GetTableUUID(),
	                                     true);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::Alter(DuckLakeTransaction &transaction, AlterTableInfo &info) {
	switch (info.alter_table_type) {
	case AlterTableType::RENAME_TABLE:
		return AlterTable(transaction, info.Cast<RenameTableInfo>());
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
