//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_data_file.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_stats.hpp"

namespace duckdb {

struct DuckLakeDataFile {
	string file_name;
	idx_t row_count;
	idx_t file_size_bytes;
	idx_t footer_size;
	map<idx_t, DuckLakeColumnStats> column_stats;
};

} // namespace duckdb
