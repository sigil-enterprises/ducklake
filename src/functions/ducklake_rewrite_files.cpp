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
#include "storage/ducklake_rewrite.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Compaction Operator
//===--------------------------------------------------------------------===//
DuckLakeRewrite::DuckLakeRewrite(PhysicalPlan &physical_plan, const vector<LogicalType> &types,
                                 DuckLakeTableEntry &table, vector<DuckLakeFileListEntry> source_files_p,
                                 string encryption_key_p, optional_idx partition_id, vector<string> partition_values_p,
                                 optional_idx row_id_start, PhysicalOperator &child)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, types, 0), table(table),
      source_files(std::move(source_files_p)), encryption_key(std::move(encryption_key_p)), partition_id(partition_id),
      partition_values(std::move(partition_values_p)), row_id_start(row_id_start) {
	children.push_back(child);
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DuckLakeRewrite::GetData(ExecutionContext &context, DataChunk &chunk,
                                          OperatorSourceInput &input) const {
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSinkState> DuckLakeRewrite::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DuckLakeInsertGlobalState>(table);
}

SinkResultType DuckLakeRewrite::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();
	DuckLakeInsert::AddWrittenFiles(global_state, chunk, encryption_key, partition_id);
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DuckLakeRewrite::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                           OperatorSinkFinalizeInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();

	if (global_state.written_files.size() != 1) {
		throw InternalException("DuckLakeCompaction - expected a single output file");
	}
	// set the partition values correctly
	for (auto &file : global_state.written_files) {
		for (idx_t col_idx = 0; col_idx < partition_values.size(); col_idx++) {
			DuckLakeFilePartition file_partition_info;
			file_partition_info.partition_column_idx = col_idx;
			file_partition_info.partition_value = partition_values[col_idx];
			file.partition_values.push_back(std::move(file_partition_info));
		}
	}

	DuckLakeRewriteEntry compaction_entry;
	compaction_entry.row_id_start = row_id_start;
	compaction_entry.source_files = source_files;
	compaction_entry.written_file = global_state.written_files[0];

	auto &transaction = DuckLakeTransaction::Get(context, global_state.table.catalog);
	// transaction.AddCompaction(global_state.table.GetTableId(), std::move(compaction_entry));
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DuckLakeRewrite::GetName() const {
	return "DUCKLAKE_REWRITE";
}

//===--------------------------------------------------------------------===//
// Logical Operator
//===--------------------------------------------------------------------===//
class DuckLakeLogicalRewrite : public LogicalExtensionOperator {
public:
	DuckLakeLogicalRewrite(idx_t table_index, DuckLakeTableEntry &table, vector<DuckLakeFileListEntry> source_files_p,
	                       string encryption_key_p, optional_idx partition_id, vector<string> partition_values_p,
	                       optional_idx row_id_start)
	    : table_index(table_index), table(table), source_files(std::move(source_files_p)),
	      encryption_key(std::move(encryption_key_p)), partition_id(partition_id),
	      partition_values(std::move(partition_values_p)), row_id_start(row_id_start) {
	}

	idx_t table_index;
	DuckLakeTableEntry &table;
	vector<DuckLakeFileListEntry> source_files;
	string encryption_key;
	optional_idx partition_id;
	vector<string> partition_values;
	optional_idx row_id_start;

public:
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		return planner.Make<DuckLakeRewrite>(types, table, std::move(source_files), std::move(encryption_key),
		                                     partition_id, std::move(partition_values), row_id_start, child);
	}

	string GetExtensionName() const override {
		return "ducklake";
	}
	vector<ColumnBinding> GetColumnBindings() override {
		vector<ColumnBinding> result;
		result.emplace_back(table_index, 0);
		return result;
	}

	void ResolveTypes() override {
		types = {LogicalType::BOOLEAN};
	}
};

//===--------------------------------------------------------------------===//
// Rewrite Command Generator
//===--------------------------------------------------------------------===//
class DuckLakeRewriter {
public:
	DuckLakeRewriter(ClientContext &context, DuckLakeCatalog &catalog, DuckLakeTransaction &transaction, Binder &binder,
	                 TableIndex table_id);

	unique_ptr<LogicalOperator> GenerateRewriter(DuckLakeTableEntry &table, double delete_threshold);
	unique_ptr<LogicalOperator> GenerateRewriterCommand(DuckLakeTableEntry &table,
	                                                    vector<DuckLakeFileListEntry> source_files);

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

unique_ptr<LogicalOperator> DuckLakeRewriter::GenerateRewriter(DuckLakeTableEntry &table, double delete_threshold) {
	auto &metadata_manager = transaction.GetMetadataManager();
	auto files = metadata_manager.GetFilesForRewrite(table, delete_threshold);
	if (files.empty()) {
		return nullptr;
	}
	// Otherwise, we do have files to rewrite
	return GenerateRewriterCommand(table, files);
}

unique_ptr<LogicalOperator> DuckLakeRewriter::GenerateRewriterCommand(DuckLakeTableEntry &table,
                                                                      vector<DuckLakeFileListEntry> source_files) {
	auto table_idx = binder.GenerateTableIndex();
	unique_ptr<FunctionData> bind_data;
	EntryLookupInfo info(CatalogType::TABLE_ENTRY, table.name);
	auto scan_function = table.GetScanFunction(context, bind_data, info);

	bool files_are_adjacent = true;

	auto &multi_file_bind_data = bind_data->Cast<MultiFileBindData>();
	auto &read_info = scan_function.function_info->Cast<DuckLakeFunctionInfo>();
	multi_file_bind_data.file_list = make_uniq<DuckLakeMultiFileList>(read_info, source_files);

	// generate the LogicalGet
	auto &columns = table.GetColumns();
	auto table_path = table.DataPath();
	string data_path;

	DuckLakeCopyInput copy_input(context, table, data_path);

	copy_input.partition_data = nullptr;
	copy_input.virtual_columns = InsertVirtualColumns::WRITE_SNAPSHOT_ID;

	auto copy_options = DuckLakeInsert::GetCopyOptions(context, copy_input);

	auto virtual_columns = table.GetVirtualColumns();
	auto ducklake_scan =
	    make_uniq<LogicalGet>(table_idx, std::move(scan_function), std::move(bind_data), copy_options.expected_types,
	                          copy_options.names, std::move(virtual_columns));
	auto &column_ids = ducklake_scan->GetMutableColumnIds();
	for (idx_t i = 0; i < columns.PhysicalColumnCount(); i++) {
		column_ids.emplace_back(i);
	}
	column_ids.emplace_back(DuckLakeMultiFileReader::COLUMN_IDENTIFIER_SNAPSHOT_ID);

	// generate the LogicalCopyToFile
	auto copy = make_uniq<LogicalCopyToFile>(std::move(copy_options.copy_function), std::move(copy_options.bind_data),
	                                         std::move(copy_options.info));

	auto &fs = FileSystem::GetFileSystem(context);
	copy->file_path = copy_options.filename_pattern.CreateFilename(fs, copy_options.file_path, "parquet", 0);
	copy->use_tmp_file = copy_options.use_tmp_file;
	copy->filename_pattern = std::move(copy_options.filename_pattern);
	copy->file_extension = std::move(copy_options.file_extension);
	copy->overwrite_mode = copy_options.overwrite_mode;
	copy->per_thread_output = copy_options.per_thread_output;
	copy->file_size_bytes = copy_options.file_size_bytes;
	copy->rotate = copy_options.rotate;
	copy->return_type = copy_options.return_type;

	copy->partition_output = copy_options.partition_output;
	copy->write_partition_columns = copy_options.write_partition_columns;
	copy->write_empty_file = false;
	copy->partition_columns = std::move(copy_options.partition_columns);
	copy->names = std::move(copy_options.names);
	copy->expected_types = std::move(copy_options.expected_types);
	copy->preserve_order = PreserveOrderType::PRESERVE_ORDER;
	copy->file_size_bytes = optional_idx();
	copy->rotate = false;

	copy->children.push_back(std::move(ducklake_scan));

	optional_idx target_row_id_start;
	optional_idx partition_id;
	vector<string> partition_values;

	auto compaction = make_uniq<DuckLakeLogicalRewrite>(binder.GenerateTableIndex(), table, std::move(source_files),
	                                                    std::move(copy_input.encryption_key), partition_id,
	                                                    std::move(partition_values), target_row_id_start);
	compaction->children.push_back(std::move(copy));
	return std::move(compaction);
}

} // namespace duckdb
