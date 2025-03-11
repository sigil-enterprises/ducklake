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
	idx_t record_count;
	idx_t table_size_bytes;
	map<idx_t, DuckLakeColumnStats> column_stats;
};

} // namespace duckdb
