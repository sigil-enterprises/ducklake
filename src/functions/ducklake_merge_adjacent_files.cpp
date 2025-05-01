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

namespace duckdb {

//===--------------------------------------------------------------------===//
// Compaction Operator
//===--------------------------------------------------------------------===//
DuckLakeCompaction::DuckLakeCompaction(const vector<LogicalType> &types, DuckLakeTableEntry &table, vector<DuckLakeCompactionFileEntry> source_files_p, string encryption_key_p, optional_idx partition_id, vector<string> partition_values_p, PhysicalOperator &child) :
	PhysicalOperator(PhysicalOperatorType::EXTENSION, types, 0), table(table), source_files(std::move(source_files_p)), encryption_key(std::move(encryption_key_p)), partition_id(partition_id), partition_values(std::move(partition_values_p)) {
	children.push_back(child);
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DuckLakeCompaction::GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSinkState> DuckLakeCompaction::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DuckLakeInsertGlobalState>(table);
}


SinkResultType DuckLakeCompaction::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();
	DuckLakeInsert::AddWrittenFiles(global_state, chunk, encryption_key, partition_id);
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DuckLakeCompaction::Finalize(Pipeline &pipeline, Event &event, ClientContext &context, OperatorSinkFinalizeInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();

	if (global_state.written_files.size() != 1) {
		throw InternalException("DuckLakeCompaction - expected a single output file");
	}
	// set the partition values correctly
	for(auto &file : global_state.written_files) {
		for(idx_t col_idx = 0; col_idx < partition_values.size(); col_idx++) {
			DuckLakeFilePartition file_partition_info;
			file_partition_info.partition_column_idx = col_idx;
			file_partition_info.partition_value = partition_values[col_idx];
			file.partition_values.push_back(std::move(file_partition_info));
		}
	}

	DuckLakeCompactionEntry compaction_entry;
	compaction_entry.row_id_start = source_files[0].file.row_id_start;
	compaction_entry.source_files = source_files;
	compaction_entry.written_file = std::move(global_state.written_files[0]);

	auto &transaction = DuckLakeTransaction::Get(context, global_state.table.catalog);
	transaction.AddCompaction(global_state.table.GetTableId(), std::move(compaction_entry));
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DuckLakeCompaction::GetName() const {
	return "DUCKLAKE_COMPACTION";
}

//===--------------------------------------------------------------------===//
// Logical Operator
//===--------------------------------------------------------------------===//
class DuckLakeLogicalCompaction : public LogicalExtensionOperator {
public:
	DuckLakeLogicalCompaction(idx_t table_index, DuckLakeTableEntry &table, vector<DuckLakeCompactionFileEntry> source_files_p, string encryption_key_p, optional_idx partition_id, vector<string> partition_values_p) :
	    table_index(table_index), table(table), source_files(std::move(source_files_p)), encryption_key(std::move(encryption_key_p)), partition_id(partition_id), partition_values(std::move(partition_values_p)) {
	}

	idx_t table_index;
	DuckLakeTableEntry &table;
	vector<DuckLakeCompactionFileEntry> source_files;
	string encryption_key;
	optional_idx partition_id;
	vector<string> partition_values;

public:
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		return planner.Make<DuckLakeCompaction>(types, table, std::move(source_files), std::move(encryption_key), partition_id, std::move(partition_values), child);
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
		types = { LogicalType::BOOLEAN };
	}
};

//===--------------------------------------------------------------------===//
// Compaction Command Generator
//===--------------------------------------------------------------------===//
class DuckLakeCompactor {
public:
	DuckLakeCompactor(ClientContext &context, DuckLakeCatalog &catalog, DuckLakeTransaction &transaction, Binder &binder, DuckLakeTableEntry &table);

	void GenerateCompactions(vector<unique_ptr<LogicalOperator>> &compactions);
	unique_ptr<LogicalOperator> GenerateCompactionCommand(vector<DuckLakeCompactionFileEntry> source_files);

private:
	ClientContext &context;
	DuckLakeCatalog &catalog;
	DuckLakeTransaction &transaction;
	Binder &binder;
	DuckLakeTableEntry &table;
};

DuckLakeCompactor::DuckLakeCompactor(ClientContext &context, DuckLakeCatalog &catalog, DuckLakeTransaction &transaction, Binder &binder, DuckLakeTableEntry &table) :
	context(context), catalog(catalog), transaction(transaction), binder(binder), table(table) {}

void DuckLakeCompactor::GenerateCompactions(vector<unique_ptr<LogicalOperator>> &compactions) {
	auto &metadata_manager = transaction.GetMetadataManager();
	auto files = metadata_manager.GetFilesForCompaction(table.GetTableId());

	// FIXME: we cannot vacuum across different partitions
	// FIXME: we cannot vacuum if the files have different schemas (schema_version needs to be the same across all snapshots)
	// target file size: 10MB
	static constexpr const idx_t TARGET_FILE_SIZE = 10000000;
	// simple compaction: iterate over the files and find adjacent files that can be merged
	for(idx_t file_idx = 0; file_idx < files.size(); file_idx++) {
		// check if we can merge this file with subsequent files
		idx_t current_file_size = 0;
		idx_t compaction_idx;
		for(compaction_idx = file_idx; compaction_idx < files.size(); compaction_idx++) {
			if (current_file_size >= TARGET_FILE_SIZE) {
				// we hit the target size already - stop
				break;
			}
			auto &candidate = files[compaction_idx];
			if (!candidate.delete_files.empty()) {
				// FIXME: files with deletions cannot be merged currently
				break;
			}
			if (!candidate.partial_files.empty()) {
				// the file already has partial files - we can only accept this as a candidate if it is the first file
				if (compaction_idx != file_idx) {
					// not the first file - we cannot compact this file together with the existing file
					break;
				}
			}
			if (candidate.file.partition_id != files[file_idx].file.partition_id ||
			    candidate.file.partition_values != files[file_idx].file.partition_values) {
				// this file is partitioned differently from the previous file - it cannot be compacted together
				break;
			}
			// FIXME: take deletions into account here once we support vacuuming deletions
			idx_t file_size = candidate.file.data.file_size_bytes;
			if (file_size >= TARGET_FILE_SIZE * 2) {
				// don't consider merging if the file is larger than twice the target size
				break;
			}
			if (compaction_idx > file_idx) {
				// rows must be adjacent to qualify for compaction
				auto &prev_file = files[compaction_idx - 1];
				if (prev_file.file.row_id_start + prev_file.file.row_count != candidate.file.row_id_start) {
					break;
				}
			}
			// this file can be compacted along with the neighbors
			current_file_size += file_size;
		}
		idx_t compaction_file_count = compaction_idx - file_idx;
		if (compaction_file_count > 1) {
			vector<DuckLakeCompactionFileEntry> compaction_files;
			for(idx_t i = 0; i < compaction_file_count; i++) {
				compaction_files.push_back(std::move(files[file_idx + i]));
			}
			compactions.push_back(GenerateCompactionCommand(std::move(compaction_files)));
			file_idx += compaction_file_count - 1;
		}
	}
}

unique_ptr<LogicalOperator> DuckLakeCompactor::GenerateCompactionCommand(vector<DuckLakeCompactionFileEntry> source_files) {
	// FIXME: get the table state at the specified snapshot
	// get the table scan function
	auto table_idx = binder.GenerateTableIndex();
	unique_ptr<FunctionData> bind_data;
	EntryLookupInfo info(CatalogType::TABLE_ENTRY, table.name);
	auto scan_function = table.GetScanFunction(context, bind_data, info);

	auto partition_id = source_files[0].file.partition_id;
	auto partition_values = source_files[0].file.partition_values;

	// set the files to scan as only the files we are trying to compact
	vector<DuckLakeFileListEntry> files_to_scan;
	for(auto &source : source_files) {
		DuckLakeFileListEntry result;
		result.file = source.file.data;
		result.row_id_start = source.file.row_id_start;
		result.snapshot_id = source.file.begin_snapshot;
		if (!source.delete_files.empty()) {
			throw InternalException("FIXME: compact deletions");
		}
		files_to_scan.push_back(std::move(result));
	}
	auto &multi_file_bind_data = bind_data->Cast<MultiFileBindData>();
	auto &read_info = scan_function.function_info->Cast<DuckLakeFunctionInfo>();
	multi_file_bind_data.file_list = make_uniq<DuckLakeMultiFileList>(transaction, read_info, std::move(files_to_scan));

	// generate the LogicalGet
	auto &columns = table.GetColumns();
	string encryption_key = catalog.GenerateEncryptionKey(context);
	auto copy_options = DuckLakeInsert::GetCopyOptions(context, columns, nullptr, table.GetFieldData(), table.DataPath(), encryption_key, InsertVirtualColumns::WRITE_SNAPSHOT_ID);

	auto virtual_columns = table.GetVirtualColumns();
	auto ducklake_scan = make_uniq<LogicalGet>(table_idx, std::move(scan_function), std::move(bind_data), copy_options.expected_types, copy_options.names, std::move(virtual_columns));
	auto &column_ids = ducklake_scan->GetMutableColumnIds();
	for(idx_t i = 0; i < columns.PhysicalColumnCount(); i++) {
		column_ids.emplace_back(i);
	}
	column_ids.emplace_back(DuckLakeMultiFileReader::COLUMN_IDENTIFIER_SNAPSHOT_ID);

	// generate the LogicalCopyToFile
	auto copy = make_uniq<LogicalCopyToFile>(std::move(copy_options.copy_function), std::move(copy_options.bind_data), std::move(copy_options.info));

	copy->file_path = std::move(copy_options.file_path);
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
	copy->write_empty_file = copy_options.write_empty_file;
	copy->partition_columns = std::move(copy_options.partition_columns);
	copy->names = std::move(copy_options.names);
	copy->expected_types = std::move(copy_options.expected_types);
	copy->preserve_order = PreserveOrderType::PRESERVE_ORDER;

	copy->children.push_back(std::move(ducklake_scan));

	// followed by the compaction operator (that writes the results back to the
	auto compaction = make_uniq<DuckLakeLogicalCompaction>(binder.GenerateTableIndex(), table, std::move(source_files), std::move(encryption_key), partition_id, std::move(partition_values));
	compaction->children.push_back(std::move(copy));
	return std::move(compaction);
}

//===--------------------------------------------------------------------===//
// Function
//===--------------------------------------------------------------------===//
unique_ptr<LogicalOperator> MergeAdjacentFilesBind(ClientContext &context, TableFunctionBindInput &input, idx_t bind_index, vector<string> &return_names) {
	// bind operator ->
	// LogicalSetOperation (LogicalGet -> LogicalCopyToFile)
	// COPY (....) TO ... UNION ALL COPY (...) TO ... ....
	// FIXME: we cannot merge files if they do not all have the same schema?
	// FIXME get list of catalogs - if they do not have the same schema version
	// gather a list of files to compact
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);

	// try to compact all tables
	vector<unique_ptr<LogicalOperator>> compactions;
	auto schemas = ducklake_catalog.GetSchemas(context);
	for(auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			DuckLakeCompactor compactor(context, ducklake_catalog, transaction, *input.binder, entry.Cast<DuckLakeTableEntry>());
			compactor.GenerateCompactions(compactions);
		});
	}
	return_names.push_back("Success");
	if (compactions.empty()) {
		// nothing to compact - generate empty result
		throw InternalException("FIXME: merge adjacent files");
	}
	if (compactions.size() == 1) {
		compactions[0]->Cast<DuckLakeLogicalCompaction>().table_index = bind_index;
		return std::move(compactions[0]);
	}
	auto union_op = input.binder->UnionOperators(std::move(compactions));
	union_op->Cast<LogicalSetOperation>().table_index = bind_index;
	return union_op;
}

DuckLakeMergeAdjacentFilesFunction::DuckLakeMergeAdjacentFilesFunction()
	: TableFunction("ducklake_merge_adjacent_files", {LogicalType::VARCHAR}, nullptr, nullptr, nullptr) {
	bind_operator = MergeAdjacentFilesBind;
}

} // namespace duckdb
