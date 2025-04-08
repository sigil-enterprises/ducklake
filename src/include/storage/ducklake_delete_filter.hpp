//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_delete_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_multi_file_reader.hpp"


namespace duckdb {

class DuckLakeDeleteFilter : public DeleteFilter {
public:
	vector<idx_t> deleted_rows;

	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) override;
	static unique_ptr<DeleteFilter> Create(ClientContext &context, const string &delete_file_path);
};


} // namespace duckdb
