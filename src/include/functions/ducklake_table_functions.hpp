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

struct MetadataBindData : public TableFunctionData {
	MetadataBindData() {
	}

	vector<vector<Value>> rows;
};

class BaseMetadataFunction : public TableFunction {
public:
	BaseMetadataFunction(string name, table_function_bind_t bind);
};

class DuckLakeSnapshotsFunction : public BaseMetadataFunction {
public:
	DuckLakeSnapshotsFunction();
};

} // namespace duckdb
