#include "../include/functions/ducklake_table_functions.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_insert.hpp"
#include "storage/ducklake_multi_file_reader.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "storage/ducklake_compaction.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "storage/ducklake_multi_file_list.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "fmt/format.h"

namespace duckdb {

unique_ptr<LogicalOperator> RewriteFilesBind(ClientContext &context, TableFunctionBindInput &input,
                                                   idx_t bind_index, vector<string> &return_names) {

	return nullptr;
}

DuckLakeRewriteDataFilesFunction::DuckLakeRewriteDataFilesFunction()
    : TableFunction("ducklake_rewrite_data_files", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, nullptr, nullptr) {
	bind_operator = RewriteFilesBind;
	named_parameters["delete_threshold"] = LogicalType::DOUBLE;
}

} // namespace duckdb

