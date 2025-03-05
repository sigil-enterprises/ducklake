//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_scan/delta_multi_file_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file_reader.hpp"

namespace duckdb {

class DuckLakeMultiFileList;

struct DuckLakeMultiFileReaderGlobalState : public MultiFileReaderGlobalState {
	DuckLakeMultiFileReaderGlobalState(vector<LogicalType> extra_columns_p,
	                                   optional_ptr<const MultiFileList> file_list_p)
	    : MultiFileReaderGlobalState(extra_columns_p, file_list_p) {
	}
	//! The idx of the file number column in the result chunk
	idx_t delta_file_number_idx = DConstants::INVALID_INDEX;
	//! The idx of the file_row_number column in the result chunk
	idx_t file_row_number_idx = DConstants::INVALID_INDEX;
};

struct DuckLakeMultiFileReader : public MultiFileReader {
	static unique_ptr<MultiFileReader> CreateInstance(const TableFunction &table_function);
	//! Return a DuckLakeMultiFileList
	shared_ptr<MultiFileList> CreateFileList(ClientContext &context, const vector<string> &paths,
	                                         FileGlobOptions options) override;

	//! Override the regular parquet bind using the MultiFileReader Bind. The bind from these are what DuckDB's file
	//! readers will try read
	bool Bind(MultiFileReaderOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
	          vector<string> &names, MultiFileReaderBindData &bind_data) override;

	//! Override the Options bind
	void BindOptions(MultiFileReaderOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
	                 vector<string> &names, MultiFileReaderBindData &bind_data) override;

	void CreateColumnMapping(const string &file_name, const vector<MultiFileReaderColumnDefinition> &local_columns,
	                         const vector<MultiFileReaderColumnDefinition> &global_columns,
	                         const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
	                         const MultiFileReaderBindData &bind_data, const string &initial_file,
	                         optional_ptr<MultiFileReaderGlobalState> global_state) override;

	unique_ptr<MultiFileReaderGlobalState>
	InitializeGlobalState(ClientContext &context, const MultiFileReaderOptions &file_options,
	                      const MultiFileReaderBindData &bind_data, const MultiFileList &file_list,
	                      const vector<MultiFileReaderColumnDefinition> &global_columns,
	                      const vector<ColumnIndex> &global_column_ids) override;

	void FinalizeBind(const MultiFileReaderOptions &file_options, const MultiFileReaderBindData &options,
	                  const string &filename, const vector<MultiFileReaderColumnDefinition> &local_columns,
	                  const vector<MultiFileReaderColumnDefinition> &global_columns,
	                  const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
	                  ClientContext &context, optional_ptr<MultiFileReaderGlobalState> global_state) override;

	//! Override the FinalizeChunk method
	void FinalizeChunk(ClientContext &context, const MultiFileReaderBindData &bind_data,
	                   const MultiFileReaderData &reader_data, DataChunk &chunk,
	                   optional_ptr<MultiFileReaderGlobalState> global_state) override;

	//! Override the ParseOption call to parse delta_scan specific options
	bool ParseOption(const string &key, const Value &val, MultiFileReaderOptions &options,
	                 ClientContext &context) override;
};

} // namespace duckdb