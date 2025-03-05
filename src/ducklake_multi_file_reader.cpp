#include "ducklake_multi_file_list.hpp"
#include "ducklake_multi_file_reader.hpp"

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

namespace duckdb {

unique_ptr<MultiFileReader> DuckLakeMultiFileReader::CreateInstance(const TableFunction &table_function) {
	throw InternalException("Unimplemented multifilereader");
}

shared_ptr<MultiFileList> DuckLakeMultiFileReader::CreateFileList(ClientContext &context, const vector<string> &paths,
                                                                  FileGlobOptions options) {
	throw InternalException("Unimplemented multifilereader");
}

bool DuckLakeMultiFileReader::Bind(MultiFileReaderOptions &options, MultiFileList &files,
                                   vector<LogicalType> &return_types, vector<string> &names,
                                   MultiFileReaderBindData &bind_data) {
	throw InternalException("Unimplemented multifilereader");
}

//! Override the Options bind
void DuckLakeMultiFileReader::BindOptions(MultiFileReaderOptions &options, MultiFileList &files,
                                          vector<LogicalType> &return_types, vector<string> &names,
                                          MultiFileReaderBindData &bind_data) {
	throw InternalException("Unimplemented multifilereader");
}

void DuckLakeMultiFileReader::CreateColumnMapping(const string &file_name,
                                                  const vector<MultiFileReaderColumnDefinition> &local_columns,
                                                  const vector<MultiFileReaderColumnDefinition> &global_columns,
                                                  const vector<ColumnIndex> &global_column_ids,
                                                  MultiFileReaderData &reader_data,
                                                  const MultiFileReaderBindData &bind_data, const string &initial_file,
                                                  optional_ptr<MultiFileReaderGlobalState> global_state) {
	throw InternalException("Unimplemented multifilereader");
}

unique_ptr<MultiFileReaderGlobalState>
DuckLakeMultiFileReader::InitializeGlobalState(ClientContext &context, const MultiFileReaderOptions &file_options,
                                               const MultiFileReaderBindData &bind_data, const MultiFileList &file_list,
                                               const vector<MultiFileReaderColumnDefinition> &global_columns,
                                               const vector<ColumnIndex> &global_column_ids) {
	throw InternalException("Unimplemented multifilereader");
}

void DuckLakeMultiFileReader::FinalizeBind(const MultiFileReaderOptions &file_options,
                                           const MultiFileReaderBindData &options, const string &filename,
                                           const vector<MultiFileReaderColumnDefinition> &local_columns,
                                           const vector<MultiFileReaderColumnDefinition> &global_columns,
                                           const vector<ColumnIndex> &global_column_ids,
                                           MultiFileReaderData &reader_data, ClientContext &context,
                                           optional_ptr<MultiFileReaderGlobalState> global_state) {
	throw InternalException("Unimplemented multifilereader");
}

void DuckLakeMultiFileReader::FinalizeChunk(ClientContext &context, const MultiFileReaderBindData &bind_data,
                                            const MultiFileReaderData &reader_data, DataChunk &chunk,
                                            optional_ptr<MultiFileReaderGlobalState> global_state) {
	throw InternalException("Unimplemented multifilereader");
}

bool DuckLakeMultiFileReader::ParseOption(const string &key, const Value &val, MultiFileReaderOptions &options,
                                          ClientContext &context) {
	throw InternalException("Unimplemented multifilereader");
}

} // namespace duckdb