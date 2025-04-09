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
#include "storage/ducklake_delete.hpp"

namespace duckdb {

DuckLakeMultiFileReader::DuckLakeMultiFileReader(DuckLakeFunctionInfo &read_info) : read_info(read_info) {
}

DuckLakeMultiFileReader::~DuckLakeMultiFileReader(){}

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

ReaderInitializeType DuckLakeMultiFileReader::InitializeReader(
	MultiFileReaderData &reader_data, const MultiFileBindData &bind_data, const vector<MultiFileColumnDefinition> &global_columns,
	const vector<ColumnIndex> &global_column_ids, optional_ptr<TableFilterSet> table_filters,
	ClientContext &context, optional_ptr<MultiFileReaderGlobalState> global_state) {
	auto &file_list = bind_data.file_list->Cast<DuckLakeMultiFileList>();
	auto &reader = *reader_data.reader;
	auto file_idx = reader.file_list_idx.GetIndex();

	auto deleted_file = file_list.GetDeletedFile(file_idx);
	if (!deleted_file.empty()) {
		auto delete_filter = DuckLakeDeleteFilter::Create(context, deleted_file);
		if (delete_map) {
			delete_map->delete_data_map.emplace(reader.GetFileName(), delete_filter->delete_data);
		}
		reader.deletion_filter = std::move(delete_filter);
	}
	return MultiFileReader::InitializeReader(reader_data, bind_data, global_columns, global_column_ids, table_filters, context, global_state);
}

} // namespace duckdb