//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/ducklake_table_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class DuckLakeSnapshotsFunction : public TableFunction {
public:
	DuckLakeSnapshotsFunction();
};

} // namespace duckdb
