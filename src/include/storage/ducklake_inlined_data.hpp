//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_inlined_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

struct DuckLakeInlinedData {
	unique_ptr<ColumnDataCollection> data;
};

} // namespace duckdb
