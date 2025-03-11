#include "ducklake_scan.hpp"
#include "ducklake_multi_file_list.hpp"
#include "ducklake_multi_file_reader.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

DuckLakeMultiFileList::DuckLakeMultiFileList(DuckLakeTransaction &transaction, DuckLakeFunctionInfo &read_info,
                                             vector<DuckLakeDataFile> transaction_local_files_p)
    : MultiFileList(vector<string> {}, FileGlobOptions::ALLOW_EMPTY), transaction(transaction), read_info(read_info),
      read_file_list(false), transaction_local_files(std::move(transaction_local_files_p)) {
}

unique_ptr<MultiFileList> DuckLakeMultiFileList::ComplexFilterPushdown(ClientContext &context,
                                                                       const MultiFileReaderOptions &options,
                                                                       MultiFilePushdownInfo &info,
                                                                       vector<unique_ptr<Expression>> &filters) {
	return nullptr;
}

unique_ptr<MultiFileList>
DuckLakeMultiFileList::DynamicFilterPushdown(ClientContext &context, const MultiFileReaderOptions &options,
                                             const vector<string> &names, const vector<LogicalType> &types,
                                             const vector<column_t> &column_ids, TableFilterSet &filters) const {
	return nullptr;
}

vector<string> DuckLakeMultiFileList::GetAllFiles() {
	return GetFiles();
}

FileExpandResult DuckLakeMultiFileList::GetExpandResult() {
	return FileExpandResult::MULTIPLE_FILES;
}

idx_t DuckLakeMultiFileList::GetTotalFileCount() {
	return GetFiles().size();
}

unique_ptr<NodeStatistics> DuckLakeMultiFileList::GetCardinality(ClientContext &context) {
	return nullptr;
}

string DuckLakeMultiFileList::GetFile(idx_t i) {
	auto &files = GetFiles();
	if (i < files.size()) {
		return files[i];
	}
	return string();
}

const vector<string> &DuckLakeMultiFileList::GetFiles() {
	if (!read_file_list) {
		// we have not read the file list yet - read it
		// FIXME: we can do pushdown of stats into the file list here to prune it
		auto query = StringUtil::Format(R"(
SELECT path
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE table_id=%d AND {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL);
		)",
		                                read_info.table_id);
		auto result = transaction.Query(read_info.snapshot, query);

		for (auto &row : *result) {
			files.push_back(row.GetValue<string>(0));
		}
		for (auto &transaction_local_file : transaction_local_files) {
			files.push_back(transaction_local_file.file_name);
		}
		read_file_list = true;
	}
	return files;
}

} // namespace duckdb