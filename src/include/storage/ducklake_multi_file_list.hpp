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
	static constexpr const idx_t TRANSACTION_LOCAL_ID_START = 1000000000000000000ULL;

public:
	explicit DuckLakeMultiFileList(DuckLakeTransaction &transaction, DuckLakeFunctionInfo &read_info,
	                               vector<DuckLakeDataFile> transaction_local_files, string filter = string());

	unique_ptr<MultiFileList> ComplexFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                MultiFilePushdownInfo &info,
	                                                vector<unique_ptr<Expression>> &filters) override;

	unique_ptr<MultiFileList> DynamicFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                const vector<string> &names, const vector<LogicalType> &types,
	                                                const vector<column_t> &column_ids,
	                                                TableFilterSet &filters) const override;

	vector<OpenFileInfo> GetAllFiles() override;
	FileExpandResult GetExpandResult() override;
	idx_t GetTotalFileCount() override;
	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context) override;
	DuckLakeTableEntry &GetTable();
	bool HasTransactionLocalFiles() const {
		return !transaction_local_files.empty();
	}
	vector<DuckLakeFileListExtendedEntry> GetFilesExtended();
	const vector<DuckLakeFileListEntry> &GetFiles();
	const DuckLakeFileListEntry &GetFileEntry(idx_t file_idx);

	bool IsDeleteScan() const;
	const DuckLakeDeleteScanEntry &GetDeleteScanEntry(idx_t file_idx);

protected:
	//! Get the i-th expanded file
	OpenFileInfo GetFile(idx_t i) override;

private:
	void GetFilesForTable();
	void GetTableInsertions();
	void GetTableDeletions();

private:
	mutex file_lock;
	DuckLakeTransaction &transaction;
	DuckLakeFunctionInfo &read_info;
	//! The set of files to read
	vector<DuckLakeFileListEntry> files;
	bool read_file_list;
	//! The set of transaction-local files
	vector<DuckLakeDataFile> transaction_local_files;
	//! The set of delete scans, only used when scanning deleted tuples using ducklake_table_deletions
	vector<DuckLakeDeleteScanEntry> delete_scans;
	//! The filter to apply
	string filter;
};

} // namespace duckdb