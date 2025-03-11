//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_table_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ducklake_data_file.hpp"

namespace duckdb {

//! These are the global, table-wide stats
struct DuckLakeTableStats {
	idx_t record_count = 0;
	idx_t table_size_bytes = 0;
	map<idx_t, DuckLakeColumnStats> column_stats;

	void MergeStats(idx_t col_id, const DuckLakeColumnStats &file_stats);
};

} // namespace duckdb
