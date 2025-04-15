//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_delete_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "storage/ducklake_metadata_info.hpp"

namespace duckdb {

struct DuckLakeDeleteData {
	vector<idx_t> deleted_rows;

	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) const;
};

class DuckLakeDeleteFilter : public DeleteFilter {
public:
	shared_ptr<DuckLakeDeleteData> delete_data;

	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) override;
	static unique_ptr<DuckLakeDeleteFilter> Create(ClientContext &context, const DuckLakeFileData &delete_file);
	static unique_ptr<DuckLakeDeleteFilter> Create(ClientContext &context, const DuckLakeDeleteScanEntry &delete_scan);

private:
	static vector<idx_t> ScanDeleteFile(ClientContext &context, const DuckLakeFileData &delete_file);
};

} // namespace duckdb
