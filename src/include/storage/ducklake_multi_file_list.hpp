//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_multi_file_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_metadata_info.hpp"

namespace duckdb {

//! The DuckLakeMultiFileList implements the MultiFileList API to allow injecting it into the regular DuckDB parquet
//! scan
class DuckLakeMultiFileList : public MultiFileList {
public:
	explicit DuckLakeMultiFileList(DuckLakeTransaction &transaction, DuckLakeFunctionInfo &read_info,
	                               vector<DuckLakeFileListEntry> transaction_local_files, string filter = string());
	explicit DuckLakeMultiFileList(DuckLakeMultiFileList &parent, vector<DuckLakeFileListEntry> files);

	unique_ptr<MultiFileList> ComplexFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                MultiFilePushdownInfo &info,
	                                                vector<unique_ptr<Expression>> &filters) override;

	unique_ptr<MultiFileList> DynamicFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                const vector<string> &names, const vector<LogicalType> &types,
	                                                const vector<column_t> &column_ids,
	                                                TableFilterSet &filters) const override;

	vector<string> GetAllFiles() override;
	FileExpandResult GetExpandResult() override;
	idx_t GetTotalFileCount() override;
	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context) override;
	DuckLakeTableEntry &GetTable();
	bool HasTransactionLocalFiles() const {
		return !transaction_local_files.empty();
	}
	const vector<DuckLakeFileListEntry> &GetFiles();
	string GetDeletedFile(idx_t file_idx);
	static vector<DuckLakeFileListEntry> GetTransactionLocalFiles(optional_ptr<vector<DuckLakeDataFile>> files);

protected:
	//! Get the i-th expanded file
	string GetFile(idx_t i) override;

private:
	mutex file_lock;
	DuckLakeTransaction &transaction;
	DuckLakeFunctionInfo &read_info;
	//! The set of files to read
	vector<DuckLakeFileListEntry> files;
	bool read_file_list;
	//! The set of transaction-local files
	vector<DuckLakeFileListEntry> transaction_local_files;
	//! The filter to apply
	string filter;
};

} // namespace duckdb