#include "storage/ducklake_delete_filter.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

idx_t DuckLakeDeleteData::Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) const {
	auto entry = std::lower_bound(deleted_rows.begin(), deleted_rows.end(), start_row_index);
	if (entry == deleted_rows.end()) {
		// no filter found for this entry
		return count;
	}
	idx_t end_pos = start_row_index + count;
	auto delete_idx = entry - deleted_rows.begin();
	if (deleted_rows[delete_idx] > end_pos) {
		// nothing in this range is deleted - skip
		return count;
	}
	// we have deletes in this range
	result_sel.Initialize(STANDARD_VECTOR_SIZE);
	idx_t result_count = 0;
	for(idx_t i = 0; i < count; i++) {
		if (delete_idx < deleted_rows.size() && start_row_index + i == deleted_rows[delete_idx]) {
			// this row is deleted - skip
			delete_idx++;
			continue;
		}
		result_sel.set_index(result_count++, i);
	}
	return result_count;
}

idx_t DuckLakeDeleteFilter::Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) {
	return delete_data->Filter(start_row_index, count, result_sel);
}

unique_ptr<DuckLakeDeleteFilter> DuckLakeDeleteFilter::Create(ClientContext &context, const string &delete_file_path) {
	auto &instance = DatabaseInstance::GetDatabase(context);
	auto &parquet_scan_entry = ExtensionUtil::GetTableFunction(instance, "parquet_scan");
	auto &parquet_scan = parquet_scan_entry.functions.functions[0];

	// Prepare the inputs for the bind
	vector<Value> children;
	children.reserve(1);
	children.push_back(Value(delete_file_path));
	named_parameter_map_t named_params;
	vector<LogicalType> input_types;
	vector<string> input_names;

	TableFunctionRef empty;
	TableFunction dummy_table_function;
	dummy_table_function.name = "DuckLakeDeleteScan";
	TableFunctionBindInput bind_input(children, named_params, input_types, input_names, nullptr, nullptr,
	                                  dummy_table_function, empty);
	vector<LogicalType> return_types;
	vector<string> return_names;

	auto bind_data = parquet_scan.bind(context, bind_input, return_types, return_names);

	if (return_types.size() != 2 || return_types[0].id() != LogicalTypeId::VARCHAR || return_types[1].id() != LogicalTypeId::BIGINT) {
		throw InvalidInputException("Invalid schema contained in the delete file %s - expected file_name/position" , delete_file_path);
	}

	DataChunk scan_chunk;
	scan_chunk.Initialize(context, return_types);

	ThreadContext thread_context(context);
	ExecutionContext execution_context(context, thread_context, nullptr);

	vector<column_t> column_ids;
	for (idx_t i = 0; i < return_types.size(); i++) {
		column_ids.push_back(i);
	}
	TableFunctionInitInput input(bind_data.get(), column_ids, vector<idx_t>(), nullptr);
	auto global_state = parquet_scan.init_global(context, input);
	auto local_state = parquet_scan.init_local(execution_context, input, global_state.get());

	auto result = make_uniq<DuckLakeDeleteFilter>();
	result->delete_data = make_shared_ptr<DuckLakeDeleteData>();
	auto &deleted_rows = result->delete_data->deleted_rows;
	int64_t last_delete = -1;
	while(true) {
		TableFunctionInput function_input(bind_data.get(), local_state.get(), global_state.get());
		scan_chunk.Reset();
		parquet_scan.function(context, function_input, scan_chunk);

		idx_t count = scan_chunk.size();
		if (count == 0) {
			break;
		}
		UnifiedVectorFormat delete_data;
		scan_chunk.data[1].ToUnifiedFormat(count, delete_data);

		auto row_ids = UnifiedVectorFormat::GetData<int64_t>(delete_data);
		for (idx_t i = 0; i < count; i++) {
			auto idx = delete_data.sel->get_index(i);
			if (!delete_data.validity.RowIsValid(idx)) {
				throw InvalidInputException("Invalid delete data - delete data cannot have NULL values");
			}
			auto &row_id = row_ids[idx];
			if (row_id <= last_delete) {
				throw InvalidInputException("Invalid delete data - row ids must be sorted and strictly increasing - but found %d after %d", row_id, last_delete);
			}

			deleted_rows.push_back(row_id);
			last_delete = row_id;
		}
	}
	return result;

}

}
