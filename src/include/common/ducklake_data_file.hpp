//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_data_file.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_stats.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

struct DuckLakeDataFile {
	string file_name;
	idx_t row_count;
	idx_t file_size_bytes;
	idx_t footer_size;
	optional_idx partition_id;
	map<idx_t, DuckLakeColumnStats> column_stats;
};

} // namespace duckdb
