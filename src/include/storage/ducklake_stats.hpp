//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"

namespace duckdb {
class BaseStatistics;

struct DuckLakeColumnStats {
	explicit DuckLakeColumnStats(LogicalType type_p) : type(std::move(type_p)) {
	}

	LogicalType type;
	string min;
	string max;
	idx_t null_count = 0;
	bool has_min = false;
	bool has_max = false;
	bool has_null_count = false;
	bool any_valid = true;

public:
	unique_ptr<BaseStatistics> ToStats() const;

private:
	unique_ptr<BaseStatistics> CreateNumericStats() const;
};

//! These are the global, table-wide stats
struct DuckLakeTableStats {
	idx_t record_count = 0;
	idx_t table_size_bytes = 0;
	map<idx_t, DuckLakeColumnStats> column_stats;

	void MergeStats(idx_t col_id, const DuckLakeColumnStats &file_stats);
};

struct DuckLakeStats {
	unordered_map<idx_t, unique_ptr<DuckLakeTableStats>> table_stats;
};

} // namespace duckdb
