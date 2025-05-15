//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_inlined_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "storage/ducklake_stats.hpp"
#include "common/index.hpp"

namespace duckdb {

struct DuckLakeInlinedData {
	unique_ptr<ColumnDataCollection> data;
	map<FieldIndex, DuckLakeColumnStats> column_stats;
};

} // namespace duckdb
