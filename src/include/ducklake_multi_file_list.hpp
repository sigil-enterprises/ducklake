//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_multi_file_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file_reader.hpp"

namespace duckdb {

struct DuckLakeFileMetaData {
	DuckLakeFileMetaData() {};

	// No copying pls
	DuckLakeFileMetaData(const DuckLakeFileMetaData &) = delete;
	DuckLakeFileMetaData &operator=(const DuckLakeFileMetaData &) = delete;
};

//! The DuckLakeMultiFileList implements the MultiFileList API to allow injecting it into the regular DuckDB parquet
//! scan
class DuckLakeMultiFileList : public MultiFileList {
public:
	unique_ptr<MultiFileList> ComplexFilterPushdown(ClientContext &context, const MultiFileReaderOptions &options,
	                                                MultiFilePushdownInfo &info,
	                                                vector<unique_ptr<Expression>> &filters) override;

	unique_ptr<MultiFileList> DynamicFilterPushdown(ClientContext &context, const MultiFileReaderOptions &options,
	                                                const vector<string> &names, const vector<LogicalType> &types,
	                                                const vector<column_t> &column_ids,
	                                                TableFilterSet &filters) const override;

	vector<string> GetAllFiles() override;
	FileExpandResult GetExpandResult() override;
	idx_t GetTotalFileCount() override;
	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context) override;

protected:
	//! Get the i-th expanded file
	string GetFile(idx_t i) override;
};

} // namespace duckdb