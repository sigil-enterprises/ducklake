//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {
class DuckLakeMultiFileList;

class DuckLakeFunctions {
public:
	//! Table Functions
	static TableFunction GetDuckLakeScanFunction(DatabaseInstance &instance);
};

struct DuckLakeFunctionInfo : public TableFunctionInfo {
	string table_name;
	vector<string> column_names;
	vector<LogicalType> column_types;
};

} // namespace duckdb
