#include "ducklake_table_entry.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "ducklake_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

DuckLakeTableEntry::DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       idx_t table_id, string table_uuid_p)
    : TableCatalogEntry(catalog, schema, info), table_id(table_id), table_uuid(std::move(table_uuid_p)) {
}

unique_ptr<BaseStatistics> DuckLakeTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction DuckLakeTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto function = DuckLakeFunctions::GetDuckLakeScanFunction(*context.db);

	// Copy over the internal kernel snapshot
	auto function_info = make_shared_ptr<DuckLakeFunctionInfo>();

	function_info->snapshot = nullptr;
	function_info->table_name = string();
	function.function_info = std::move(function_info);

	vector<Value> inputs {};
	named_parameter_map_t param_map;
	vector<LogicalType> return_types;
	vector<string> names;
	TableFunctionRef empty_ref;

	TableFunctionBindInput bind_input(inputs, param_map, return_types, names, nullptr, nullptr, function,
									  empty_ref);

	auto result = function.bind(context, bind_input, return_types, names);
	bind_data = std::move(result);

	return function;
}

TableStorageInfo DuckLakeTableEntry::GetStorageInfo(ClientContext &context) {
	throw InternalException("Unsupported function for table entry");
}

void DuckLakeTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                               LogicalUpdate &update, ClientContext &context) {
	throw InternalException("Unsupported function for table entry");
}

} // namespace duckdb
