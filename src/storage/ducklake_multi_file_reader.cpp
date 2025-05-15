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
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "storage/ducklake_delete.hpp"
#include "duckdb/function/function_binder.hpp"
#include "storage/ducklake_inlined_data_reader.hpp"

namespace duckdb {

DuckLakeMultiFileReader::DuckLakeMultiFileReader(DuckLakeFunctionInfo &read_info) : read_info(read_info) {
	row_id_column = make_uniq<MultiFileColumnDefinition>("_ducklake_internal_row_id", LogicalType::BIGINT);
	row_id_column->identifier = Value::INTEGER(MultiFileReader::ROW_ID_FIELD_ID);
	snapshot_id_column = make_uniq<MultiFileColumnDefinition>("_ducklake_internal_snapshot_id", LogicalType::BIGINT);
	snapshot_id_column->identifier = Value::INTEGER(MultiFileReader::LAST_UPDATED_SEQUENCE_NUMBER_ID);
}

DuckLakeMultiFileReader::~DuckLakeMultiFileReader() {
}

unique_ptr<MultiFileReader> DuckLakeMultiFileReader::Copy() const {
	auto result = make_uniq<DuckLakeMultiFileReader>(read_info);
	result->transaction_local_data = transaction_local_data;
	return result;
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
	transaction_local_data = transaction.GetTransactionLocalInlinedData(read_info.table_id);
	auto result = make_shared_ptr<DuckLakeMultiFileList>(transaction, read_info, std::move(transaction_local_files),
	                                                     transaction_local_data);
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

vector<MultiFileColumnDefinition> DuckLakeMultiFileReader::ColumnsFromFieldData(const DuckLakeFieldData &field_data) {
	vector<MultiFileColumnDefinition> result;
	for (auto &item : field_data.GetFieldIds()) {
		result.push_back(CreateColumnFromFieldId(*item));
	}
	return result;
}

bool DuckLakeMultiFileReader::Bind(MultiFileOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
                                   vector<string> &names, MultiFileReaderBindData &bind_data) {
	auto &field_data = read_info.table.GetFieldData();
	auto &columns = bind_data.schema;
	columns = ColumnsFromFieldData(field_data);
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

ReaderInitializeType DuckLakeMultiFileReader::InitializeReader(MultiFileReaderData &reader_data,
                                                               const MultiFileBindData &bind_data,
                                                               const vector<MultiFileColumnDefinition> &global_columns,
                                                               const vector<ColumnIndex> &global_column_ids,
                                                               optional_ptr<TableFilterSet> table_filters,
                                                               ClientContext &context,
                                                               optional_ptr<MultiFileReaderGlobalState> global_state) {
	auto &file_list = bind_data.file_list->Cast<DuckLakeMultiFileList>();
	auto &reader = *reader_data.reader;
	auto file_idx = reader.file_list_idx.GetIndex();

	if (!file_list.IsDeleteScan()) {
		// regular scan - read the deletes from the delete file (if any) and apply the max row count
		auto &file_entry = file_list.GetFileEntry(file_idx);
		if (!file_entry.delete_file.path.empty() || file_entry.max_row_count.IsValid()) {
			auto delete_filter = make_uniq<DuckLakeDeleteFilter>();
			if (!file_entry.delete_file.path.empty()) {
				delete_filter->Initialize(context, file_entry.delete_file);
			}
			if (file_entry.max_row_count.IsValid()) {
				delete_filter->SetMaxRowCount(file_entry.max_row_count.GetIndex());
			}
			if (delete_map) {
				delete_map->AddDeleteData(reader.GetFileName(), delete_filter->delete_data);
			}
			reader.deletion_filter = std::move(delete_filter);
		}
	} else {
		// delete scan - we need to read ONLY the entries that have been deleted
		auto &delete_entry = file_list.GetDeleteScanEntry(file_idx);
		auto delete_filter = make_uniq<DuckLakeDeleteFilter>();
		delete_filter->Initialize(context, delete_entry);
		reader.deletion_filter = std::move(delete_filter);
	}
	return MultiFileReader::InitializeReader(reader_data, bind_data, global_columns, global_column_ids, table_filters,
	                                         context, global_state);
}

shared_ptr<BaseFileReader> DuckLakeMultiFileReader::TryCreateInlinedDataReader(const OpenFileInfo &file) {
	if (!file.extended_info) {
		return nullptr;
	}
	auto entry = file.extended_info->options.find("inlined_data");
	if (entry == file.extended_info->options.end()) {
		return nullptr;
	}
	// this is not a file but inlined data
	entry = file.extended_info->options.find("table_name");
	if (entry == file.extended_info->options.end()) {
		// scanning transaction local inlined data
		if (!transaction_local_data) {
			throw InternalException("No transaction local data");
		}
		auto columns = DuckLakeMultiFileReader::ColumnsFromFieldData(read_info.table.GetFieldData());
		return make_shared_ptr<DuckLakeInlinedDataReader>(read_info, file, transaction_local_data, std::move(columns));
	}
	// we are reading from a table - set up the inlined data reader that will read this data when requested
	//! FIXME: we cannot just grab the field data from the table when this is not the latest inlined data table
	auto columns = DuckLakeMultiFileReader::ColumnsFromFieldData(read_info.table.GetFieldData());
	columns.insert(columns.begin(), *snapshot_id_column);
	columns.insert(columns.begin(), *row_id_column);

	auto inlined_table_name = StringValue::Get(entry->second);
	return make_shared_ptr<DuckLakeInlinedDataReader>(read_info, file, std::move(inlined_table_name),
	                                                  std::move(columns));
}

shared_ptr<BaseFileReader> DuckLakeMultiFileReader::CreateReader(ClientContext &context,
                                                                 GlobalTableFunctionState &gstate,
                                                                 const OpenFileInfo &file, idx_t file_idx,
                                                                 const MultiFileBindData &bind_data) {
	auto reader = TryCreateInlinedDataReader(file);
	if (reader) {
		return reader;
	}
	return MultiFileReader::CreateReader(context, gstate, file, file_idx, bind_data);
}

shared_ptr<BaseFileReader> DuckLakeMultiFileReader::CreateReader(ClientContext &context, const OpenFileInfo &file,
                                                                 BaseFileReaderOptions &options,
                                                                 const MultiFileOptions &file_options,
                                                                 MultiFileReaderInterface &interface) {
	auto reader = TryCreateInlinedDataReader(file);
	if (reader) {
		return reader;
	}
	return MultiFileReader::CreateReader(context, file, options, file_options, interface);
}

unique_ptr<Expression> DuckLakeMultiFileReader::GetVirtualColumnExpression(
    ClientContext &context, MultiFileReaderData &reader_data, const vector<MultiFileColumnDefinition> &local_columns,
    idx_t &column_id, const LogicalType &type, MultiFileLocalIndex local_idx,
    optional_ptr<MultiFileColumnDefinition> &global_column_reference) {
	if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
		// row id column
		// this is computed as row_id_start + file_row_number OR read from the file
		// first check if the row id is explicitly defined in this file
		for (auto &col : local_columns) {
			if (col.identifier.IsNull()) {
				continue;
			}
			if (col.identifier.GetValue<int32_t>() == MultiFileReader::ROW_ID_FIELD_ID) {
				// it is! return a reference to the global row id column so we can read it from the file directly
				global_column_reference = row_id_column.get();
				return nullptr;
			}
		}
		// get the row id start for this file
		if (!reader_data.file_to_be_opened.extended_info) {
			throw InternalException("Extended info not found for reading row id column");
		}
		auto &options = reader_data.file_to_be_opened.extended_info->options;
		auto entry = options.find("row_id_start");
		if (entry == options.end()) {
			throw InternalException("row_id_start not found for reading row id column");
		}
		auto row_id_expr = make_uniq<BoundConstantExpression>(entry->second);
		auto file_row_number = make_uniq<BoundReferenceExpression>(type, local_idx.GetIndex());

		// transform this virtual column to file_row_number
		column_id = MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER;

		// generate the addition
		vector<unique_ptr<Expression>> children;
		children.push_back(std::move(row_id_expr));
		children.push_back(std::move(file_row_number));

		FunctionBinder binder(context);
		ErrorData error;
		auto function_expr = binder.BindScalarFunction(DEFAULT_SCHEMA, "+", std::move(children), error, true, nullptr);
		if (error.HasError()) {
			error.Throw();
		}
		return function_expr;
	}
	if (column_id == COLUMN_IDENTIFIER_SNAPSHOT_ID) {
		for (auto &col : local_columns) {
			if (col.identifier.IsNull()) {
				continue;
			}
			if (col.identifier.GetValue<int32_t>() == MultiFileReader::LAST_UPDATED_SEQUENCE_NUMBER_ID) {
				// it is! return a reference to the global snapshot id column so we can read it from the file directly
				global_column_reference = snapshot_id_column.get();
				return nullptr;
			}
		}
		// get the row id start for this file
		if (!reader_data.file_to_be_opened.extended_info) {
			throw InternalException("Extended info not found for reading snapshot id column");
		}
		auto &options = reader_data.file_to_be_opened.extended_info->options;
		auto entry = options.find("snapshot_id");
		if (entry == options.end()) {
			throw InternalException("snapshot_id not found for reading snapshot_id column");
		}
		return make_uniq<BoundConstantExpression>(entry->second);
	}
	return MultiFileReader::GetVirtualColumnExpression(context, reader_data, local_columns, column_id, type, local_idx,
	                                                   global_column_reference);
}

} // namespace duckdb
