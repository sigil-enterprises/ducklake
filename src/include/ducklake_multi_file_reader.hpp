//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_scan/delta_multi_file_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file_reader.hpp"
#include "ducklake_scan.hpp"

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
	explicit DuckLakeMultiFileReader(DuckLakeFunctionInfo &read_info);

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

private:
	DuckLakeFunctionInfo &read_info;
};

} // namespace duckdb