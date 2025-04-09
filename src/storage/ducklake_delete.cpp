#include "storage/ducklake_catalog.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "storage/ducklake_scan.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "common/ducklake_data_file.hpp"
#include "storage/ducklake_multi_file_list.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "storage/ducklake_multi_file_reader.hpp"

namespace duckdb {

DuckLakeDelete::DuckLakeDelete(DuckLakeTableEntry &table, PhysicalOperator &child, shared_ptr<DuckLakeDeleteMap> delete_map_p)
    : PhysicalOperator(PhysicalOperatorType::EXTENSION, {LogicalType::BIGINT}, 1), table(table), delete_map(std::move(delete_map_p)) {
    children.push_back(child);
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
struct WrittenColumnInfo {
	WrittenColumnInfo() = default;
	WrittenColumnInfo(LogicalType type_p, int32_t field_id) : type(std::move(type_p)), field_id(field_id) {}

	LogicalType type;
	int32_t field_id;
};

class DuckLakeDeleteLocalState : public LocalSinkState {
public:
	string current_filename;
	vector<idx_t> file_row_numbers;
};

class DuckLakeDeleteGlobalState : public GlobalSinkState {
public:
	explicit DuckLakeDeleteGlobalState() {
		written_columns["file_path"] = WrittenColumnInfo(LogicalType::VARCHAR, 2147483546);
		written_columns["pos"] = WrittenColumnInfo(LogicalType::BIGINT, 2147483545);
	}

	mutex lock;
	unordered_map<string, DuckLakeDeleteFile> written_files;
	unordered_map<string, WrittenColumnInfo> written_columns;
	idx_t total_deleted_count = 0;
	unordered_map<string, vector<idx_t>> deleted_rows;

	void Flush(DuckLakeDeleteLocalState &local_state) {
	 	auto &local_entry = local_state.file_row_numbers;
		if (local_entry.empty()) {
			return;
		}
		lock_guard<mutex> guard(lock);
		auto &global_entry = deleted_rows[local_state.current_filename];
		global_entry.insert(global_entry.end(), local_entry.begin(), local_entry.end());
		total_deleted_count += local_entry.size();
		local_entry.clear();
	}
};

unique_ptr<GlobalSinkState> DuckLakeDelete::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DuckLakeDeleteGlobalState>();
}

unique_ptr<LocalSinkState> DuckLakeDelete::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<DuckLakeDeleteLocalState>();
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType DuckLakeDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeDeleteGlobalState>();
	auto &local_state = input.local_state.Cast<DuckLakeDeleteLocalState>();

	auto base_idx = chunk.ColumnCount() - 2;
	auto &filename_vector = chunk.data[base_idx];
	auto &file_row_number = chunk.data[base_idx + 1];
	if (filename_vector.GetVectorType() != VectorType::CONSTANT_VECTOR) {
		throw NotImplementedException("DuckLake Delete - Filename should be a constant");
	}
	auto filename_data = ConstantVector::GetData<string_t>(filename_vector);
	if (ConstantVector::IsNull(filename_vector)) {
		throw InternalException("Filename is NULL!?");
	}
	if (filename_data[0] != string_t(local_state.current_filename)) {
		// filename has changed - flush
		global_state.Flush(local_state);
		local_state.current_filename = filename_data[0].GetString();
	}
	// append the file row numbers
	UnifiedVectorFormat row_data;
	file_row_number.ToUnifiedFormat(chunk.size(), row_data);

	auto file_row_data = UnifiedVectorFormat::GetData<int64_t>(row_data);

	for(idx_t i = 0; i < chunk.size(); i++) {
		auto row_idx = row_data.sel->get_index(i);
		auto row_number = file_row_data[row_idx];
		local_state.file_row_numbers.push_back(row_number);
	}
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
SinkCombineResultType DuckLakeDelete::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeDeleteGlobalState>();
	auto &local_state = input.local_state.Cast<DuckLakeDeleteLocalState>();
	global_state.Flush(local_state);
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
void DuckLakeDelete::FlushDelete(ClientContext &context, DuckLakeDeleteGlobalState &global_state, const string &filename, vector<idx_t> &deleted_rows) const {
	auto &fs = FileSystem::GetFileSystem(context);
	auto delete_file_uuid = UUID::ToString(UUID::GenerateRandomUUID()) + "-delete.parquet";
	string delete_file_path = fs.JoinPath(table.DataPath(), delete_file_uuid);

	auto info = make_uniq<CopyInfo>();
	info->file_path = delete_file_path;
	info->format = "parquet";
	info->is_from = false;

    // generate the field ids to be written by the parquet writer
    // these field ids follow icebergs' ids and names for the delete files
	child_list_t<Value> values;
	values.emplace_back("file_path", Value::INTEGER(2147483546));
	values.emplace_back("pos", Value::INTEGER(2147483545));
	auto field_ids = Value::STRUCT(std::move(values));
	vector<Value> field_input;
	field_input.push_back(std::move(field_ids));
	info->options["field_ids"] = std::move(field_input);

	// get the actual copy function and bind it
	auto &copy_fun = DuckLakeFunctions::GetCopyFunction(*context.db, "parquet");

	CopyFunctionBindInput bind_input(*info);

	vector<string> names_to_write { "file_path", "pos" };
	vector<LogicalType> types_to_write { LogicalType::VARCHAR, LogicalType::BIGINT };

	auto function_data = copy_fun.function.copy_to_bind(context, bind_input, names_to_write, types_to_write);

	// generate the physical copy to file
	auto copy_return_types = GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS);
	PhysicalCopyToFile copy_to_file(copy_return_types, copy_fun.function, std::move(function_data), 1);

	copy_to_file.use_tmp_file = false;
	copy_to_file.file_path = delete_file_path;
	copy_to_file.partition_output = false;
	copy_to_file.write_empty_file = false;
	copy_to_file.file_extension = "parquet";
	copy_to_file.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
	copy_to_file.per_thread_output = false;
	copy_to_file.rotate = false;
	copy_to_file.return_type = CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
	copy_to_file.write_partition_columns = false;

	DuckLakeDeleteFile delete_file;
	// check if the file already has deletes
	auto entry = delete_map->delete_data_map.find(filename);
	if (entry != delete_map->delete_data_map.end()) {
		// deletes already exist for this file - add to deleted_rows
		auto &existing_deletes = entry->second->deleted_rows;
		deleted_rows.insert(deleted_rows.end(), existing_deletes.begin(), existing_deletes.end());

		// clear the deletes
		delete_map->delete_data_map.erase(entry);

		// set the delete file as overwriting existing deletes
		delete_file.overwrites_existing_delete = true;
	}

	// sort the to-be-inserted file numbers
	std::sort(deleted_rows.begin(), deleted_rows.end());

	// run the copy to file
	vector<LogicalType> write_types;
	write_types.push_back(LogicalType::VARCHAR);
	write_types.push_back(LogicalType::BIGINT);

	DataChunk write_chunk;
	write_chunk.Initialize(context, write_types);

	// the first vector is constant (the file name)
	Value filename_val(filename);
	write_chunk.data[0].Reference(filename_val);

	ThreadContext thread_context(context);
	ExecutionContext execution_context(context, thread_context, nullptr);
	InterruptState interrupt_state;

	// run the PhysicalCopyToFile Sink pipeline
	auto gstate = copy_to_file.GetGlobalSinkState(context);
	auto lstate = copy_to_file.GetLocalSinkState(execution_context);

	OperatorSinkInput sink_input {*gstate, *lstate, interrupt_state};
	idx_t row_count = 0;
	auto row_data = FlatVector::GetData<int64_t>(write_chunk.data[1]);
	for(idx_t i = 0; i < deleted_rows.size(); i++) {
		row_data[row_count] = deleted_rows[i];
		if (i == 0 || deleted_rows[i] != deleted_rows[i - 1]) {
			row_count++;
		}
		if (row_count >= STANDARD_VECTOR_SIZE || i + 1 == deleted_rows.size()) {
			write_chunk.SetCardinality(row_count);
			copy_to_file.Sink(execution_context, write_chunk, sink_input);
			row_count = 0;
		}
	}
	OperatorSinkCombineInput combine_input {*gstate, *lstate, interrupt_state};
	copy_to_file.Combine(execution_context, combine_input);
	copy_to_file.FinalizeInternal(context, *gstate);

	// now read the stats data
	copy_to_file.sink_state = std::move(gstate);
	auto source_state = copy_to_file.GetGlobalSourceState(context);
	auto local_state = copy_to_file.GetLocalSourceState(execution_context, *source_state);
	DataChunk stats_chunk;
	stats_chunk.Initialize(context, copy_to_file.types);

	OperatorSourceInput source_input {*source_state, *local_state, interrupt_state};
	copy_to_file.GetData(execution_context, stats_chunk, source_input);

	if (stats_chunk.size() != 1) {
		throw InternalException("Expected a single delete file to be written here");
	}
	idx_t r = 0;
	// add to the written files
	delete_file.file_name = stats_chunk.GetValue(0, r).GetValue<string>();
	delete_file.delete_count = stats_chunk.GetValue(1, r).GetValue<idx_t>();
	delete_file.file_size_bytes = stats_chunk.GetValue(2, r).GetValue<idx_t>();
	delete_file.footer_size = stats_chunk.GetValue(3, r).GetValue<idx_t>();
	global_state.written_files.emplace(filename, std::move(delete_file));
}

SinkFinalizeType DuckLakeDelete::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
 	auto &global_state = input.global_state.Cast<DuckLakeDeleteGlobalState>();

	// write out the delete rows
	for(auto &entry : global_state.deleted_rows) {
		FlushDelete(context, global_state, entry.first, entry.second);
	}
	auto &transaction = DuckLakeTransaction::Get(context, table.catalog);
	auto &fs = FileSystem::GetFileSystem(context);
	vector<DuckLakeDeleteFile> delete_files;
	for(auto &entry : global_state.written_files) {
		auto &data_file_path = entry.first;
		auto delete_file = std::move(entry.second);
		auto delete_entry = delete_map->file_index_map.find(data_file_path);
		if (delete_entry == delete_map->file_index_map.end()) {
			throw InternalException("Could not find matching file for written delete file");
		}
		auto data_file_id = delete_entry->second;
		delete_file.data_file_id = data_file_id;
		if (data_file_id.IsValid()) {
			// deleting from a committed file - add to delete files directly
			delete_files.push_back(std::move(delete_file));
		} else {
			// deleting from a transaction local file - find the file we are deleting from
			auto files = transaction.GetTransactionLocalFiles(table.GetTableId());
			bool found = false;
			for(auto &file : *files) {
				if (file.file_name == data_file_path) {
					if (file.delete_file) {
						// this file already has a transaction-local delete file - delete it
						fs.RemoveFile(file.delete_file->file_name);
					}
					file.delete_file = make_uniq<DuckLakeDeleteFile>(std::move(delete_file));
					found = true;
					break;
				}
			}
			if (!found) {
				throw InternalException("Failed to find matching transaction-local file for written delete file");
			}
			delete_file.overwrites_existing_delete = false;
		}
	}
	transaction.AddDeletes(table.GetTableId(), std::move(delete_files));

	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DuckLakeDelete::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
	auto &global_state = sink_state->Cast<DuckLakeDeleteGlobalState>();
	auto value = Value::BIGINT(global_state.total_deleted_count);
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, value);
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DuckLakeDelete::GetName() const {
	return "DUCKLAKE_DELETE";
}

InsertionOrderPreservingMap<string> DuckLakeDelete::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name;
	return result;
}

optional_ptr<PhysicalTableScan> FindDeleteSource(PhysicalOperator &plan) {
	if (plan.type == PhysicalOperatorType::TABLE_SCAN) {
		// does this emit the virtual columns?
		auto &scan = plan.Cast<PhysicalTableScan>();
		bool found = false;
		for(auto &col : scan.column_ids) {
			if (col.GetPrimaryIndex() == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
				found = true;
				break;
			}
		}
		if (!found) {
			return nullptr;
		}
		return scan;
	}
	for(auto &children : plan.children) {
		auto result = FindDeleteSource(children.get());
		if (result) {
			return result;
		}
	}
	return nullptr;
}

PhysicalOperator &DuckLakeCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &child_plan) {
    auto delete_source = FindDeleteSource(child_plan);
	auto delete_map = make_shared_ptr<DuckLakeDeleteMap>();
    if (delete_source) {
	    auto &bind_data = delete_source->bind_data->Cast<MultiFileBindData>();
	    auto &reader = bind_data.multi_file_reader->Cast<DuckLakeMultiFileReader>();
	    auto &file_list = bind_data.file_list->Cast<DuckLakeMultiFileList>();
	    auto &files = file_list.GetFiles();
	    auto &delete_file_map = delete_map->file_index_map;
	    for(auto &file : files) {
	    	delete_file_map[file.path] = file.file_id;
	    }
	    reader.delete_map = delete_map;
    }
	return planner.Make<DuckLakeDelete>(op.table.Cast<DuckLakeTableEntry>(), child_plan, std::move(delete_map));
}

} // namespace duckdb
