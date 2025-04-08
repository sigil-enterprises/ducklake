#include "storage/ducklake_multi_file_list.hpp"
#include "storage/ducklake_multi_file_reader.hpp"
#include "storage/ducklake_table_entry.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {

DuckLakeMultiFileReader::DuckLakeMultiFileReader(DuckLakeFunctionInfo &read_info) : read_info(read_info) {
}

unique_ptr<MultiFileReader> DuckLakeMultiFileReader::CreateInstance(const TableFunction &table_function) {
	auto &function_info = table_function.function_info->Cast<DuckLakeFunctionInfo>();
	auto result = make_uniq<DuckLakeMultiFileReader>(function_info);
	return std::move(result);
}

shared_ptr<MultiFileList> DuckLakeMultiFileReader::CreateFileList(ClientContext &context, const vector<string> &paths,
                                                                  FileGlobOptions options) {
	auto &transaction = DuckLakeTransaction::Get(context, read_info.table.ParentCatalog());
	auto transaction_local_files = transaction.GetTransactionLocalFiles(read_info.table_id);
	auto result = make_shared_ptr<DuckLakeMultiFileList>(transaction, read_info, std::move(transaction_local_files));
	return std::move(result);
}

MultiFileColumnDefinition CreateColumnFromFieldId(const DuckLakeFieldId &field_id) {
	MultiFileColumnDefinition column(field_id.Name(), field_id.Type());
	auto &column_data = field_id.GetColumnData();
	if (column_data.initial_default.IsNull()) {
		column.default_expression = make_uniq<ConstantExpression>(Value(field_id.Type()));
	} else {
		column.default_expression = make_uniq<ConstantExpression>(column_data.initial_default);
	}
	column.identifier = Value::INTEGER(field_id.GetFieldIndex().index);
	for (auto &child : field_id.Children()) {
		column.children.push_back(CreateColumnFromFieldId(*child));
	}
	return column;
}

bool DuckLakeMultiFileReader::Bind(MultiFileOptions &options, MultiFileList &files,
                                   vector<LogicalType> &return_types, vector<string> &names,
                                   MultiFileReaderBindData &bind_data) {
	auto &field_data = read_info.table.GetFieldData();
	auto &columns = bind_data.schema;
	for (auto &item : field_data.GetFieldIds()) {
		columns.push_back(CreateColumnFromFieldId(*item));
	}
	//	bind_data.file_row_number_idx = names.size();
	bind_data.mapping = MultiFileColumnMappingMode::BY_FIELD_ID;
	names = read_info.column_names;
	return_types = read_info.column_types;
	return true;
}

//! Override the Options bind
void DuckLakeMultiFileReader::BindOptions(MultiFileOptions &options, MultiFileList &files,
                                          vector<LogicalType> &return_types, vector<string> &names,
                                          MultiFileReaderBindData &bind_data) {
}

class DuckLakeDeleteFilter : public DeleteFilter {
public:
	vector<idx_t> deleted_rows;

	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) override {
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

	static unique_ptr<DeleteFilter> Create(ClientContext &context, const string &delete_file_path) {
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

				result->deleted_rows.push_back(row_id);
				last_delete = row_id;
			}
		}
		return result;

	}
};

ReaderInitializeType DuckLakeMultiFileReader::InitializeReader(
	MultiFileReaderData &reader_data, const MultiFileBindData &bind_data, const vector<MultiFileColumnDefinition> &global_columns,
	const vector<ColumnIndex> &global_column_ids, optional_ptr<TableFilterSet> table_filters,
	ClientContext &context, optional_ptr<MultiFileReaderGlobalState> global_state) {
	auto &file_list = bind_data.file_list->Cast<DuckLakeMultiFileList>();
	auto file_idx = reader_data.reader->file_list_idx.GetIndex();

	auto deleted_file = file_list.GetDeletedFile(file_idx);
	if (!deleted_file.empty()) {
		reader_data.reader->deletion_filter = DuckLakeDeleteFilter::Create(context, deleted_file);
	}
	return MultiFileReader::InitializeReader(reader_data, bind_data, global_columns, global_column_ids, table_filters, context, global_state);
}

} // namespace duckdb