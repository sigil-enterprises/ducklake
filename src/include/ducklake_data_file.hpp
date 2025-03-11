//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_data_file.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"

namespace duckdb {

struct DuckLakeColumnStats {
	string min;
	string max;
	idx_t null_count = 0;
	bool has_min = false;
	bool has_max = false;
	bool has_null_count = false;
};

struct DuckLakeDataFile {
	string file_name;
	idx_t row_count;
	idx_t file_size_bytes;
	idx_t footer_size;
	case_insensitive_map_t<DuckLakeColumnStats> column_stats;
};

} // namespace duckdb
