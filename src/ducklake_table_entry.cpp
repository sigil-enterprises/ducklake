#include "ducklake_table_entry.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

DuckLakeTableEntry::DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, idx_t table_id, string table_uuid_p) : TableCatalogEntry(catalog, schema, info), table_id(table_id), table_uuid(std::move(table_uuid_p)) {
}


unique_ptr<BaseStatistics> DuckLakeTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	throw InternalException("Unsupported function for table entry");
}

TableFunction DuckLakeTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	throw InternalException("Unsupported function for table entry");
}

TableStorageInfo DuckLakeTableEntry::GetStorageInfo(ClientContext &context) {
	throw InternalException("Unsupported function for table entry");
}

void DuckLakeTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
						   ClientContext &context) {
	throw InternalException("Unsupported function for table entry");
}

}
