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

unique_ptr<MultiFileList> DuckLakeMultiFileList::ComplexFilterPushdown(ClientContext &context,
                                                                       const MultiFileReaderOptions &options,
                                                                       MultiFilePushdownInfo &info,
                                                                       vector<unique_ptr<Expression>> &filters) {
	throw InternalException("Unimplemented multifilelist");
}

unique_ptr<MultiFileList>
DuckLakeMultiFileList::DynamicFilterPushdown(ClientContext &context, const MultiFileReaderOptions &options,
                                             const vector<string> &names, const vector<LogicalType> &types,
                                             const vector<column_t> &column_ids, TableFilterSet &filters) const {
	throw InternalException("Unimplemented multifilelist");
}

vector<string> DuckLakeMultiFileList::GetAllFiles() {
	throw InternalException("Unimplemented multifilelist");
}
FileExpandResult DuckLakeMultiFileList::GetExpandResult() {
	throw InternalException("Unimplemented multifilelist");
}
idx_t DuckLakeMultiFileList::GetTotalFileCount() {
	throw InternalException("Unimplemented multifilelist");
}
unique_ptr<NodeStatistics> DuckLakeMultiFileList::GetCardinality(ClientContext &context) {
	throw InternalException("Unimplemented multifilelist");
}

string DuckLakeMultiFileList::GetFile(idx_t i) {
	throw InternalException("Unimplemented multifilelist");
}

} // namespace duckdb