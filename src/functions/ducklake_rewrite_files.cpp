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

//===--------------------------------------------------------------------===//
// Rewrite Command Generator
//===--------------------------------------------------------------------===//
class DuckLakeRewriter {
public:
	DuckLakeRewriter(ClientContext &context, DuckLakeCatalog &catalog, DuckLakeTransaction &transaction,
	                  Binder &binder, TableIndex table_id);

	unique_ptr<LogicalOperator>  GenerateRewriter(DuckLakeTableEntry &table);
	unique_ptr<LogicalOperator> GenerateRewriterCommand(vector<DuckLakeRewriterFileEntry> source_files);

private:
	ClientContext &context;
	DuckLakeCatalog &catalog;
	DuckLakeTransaction &transaction;
	Binder &binder;
	TableIndex table_id;
};

DuckLakeRewriter::DuckLakeRewriter(ClientContext &context, DuckLakeCatalog &catalog, DuckLakeTransaction &transaction,
                                     Binder &binder, TableIndex table_id)
    : context(context), catalog(catalog), transaction(transaction), binder(binder), table_id(table_id) {
}


unique_ptr<LogicalOperator> RewriteFilesBind(ClientContext &context, TableFunctionBindInput &input,
                                                   idx_t bind_index, vector<string> &return_names) {

	// gather a list of files to compact
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);

	// By default, our delete threshold is 0.95 unless specified otherwise
	double delete_threshold = 0.95;
	auto delete_threshold_entry = input.named_parameters.find("delete_threshold");
	if (delete_threshold_entry != input.named_parameters.end()) {
		delete_threshold = DoubleValue::Get(delete_threshold_entry->second);
	}

	string schema;
	auto schema_entry = input.named_parameters.find("schema");
	if (schema_entry != input.named_parameters.end()) {
		schema = StringValue::Get(schema_entry->second);
	}

	auto table_name = StringValue::Get(input.inputs[1]);
	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, table_name, nullptr, QueryErrorContext());
	auto table_entry = catalog.GetEntry(context, schema, table_lookup, OnEntryNotFound::THROW_EXCEPTION);
	auto &table = table_entry->Cast<DuckLakeTableEntry>();

	// Now we try to rewrite the ducklake table
	DuckLakeRewriter rewriter(context, ducklake_catalog, transaction, *input.binder, table.GetTableId());
	auto op = rewriter.GenerateRewriter(table);
	return_names.push_back("Success");
	if (!op) {
		// nothing to rewrite - generate an empty result
		vector<ColumnBinding> bindings;
		vector<LogicalType> return_types;
		bindings.emplace_back(bind_index, 0);
		return_types.emplace_back(LogicalType::BOOLEAN);
		return make_uniq<LogicalEmptyResult>(std::move(return_types), std::move(bindings));
	}

	op->Cast<DuckLakeLogicalCompaction>().table_index = bind_index;
	return std::move(op);

}

DuckLakeRewriteDataFilesFunction::DuckLakeRewriteDataFilesFunction()
    : TableFunction("ducklake_rewrite_data_files", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, nullptr, nullptr) {
	bind_operator = RewriteFilesBind;
	named_parameters["delete_threshold"] = LogicalType::DOUBLE;
	named_parameters["schema"] = LogicalType::VARCHAR;
}

} // namespace duckdb

