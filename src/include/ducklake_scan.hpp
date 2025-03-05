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
	shared_ptr<DuckLakeMultiFileList> snapshot;
	string expected_path;
	string table_name;
};

} // namespace duckdb
