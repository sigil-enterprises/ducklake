//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "ducklake_snapshot.hpp"

namespace duckdb {
class DuckLakeMultiFileList;
class DuckLakeTableEntry;

class DuckLakeFunctions {
public:
	//! Table Functions
	static TableFunction GetDuckLakeScanFunction(DatabaseInstance &instance);
};

struct DuckLakeFunctionInfo : public TableFunctionInfo {
	DuckLakeFunctionInfo(DuckLakeTableEntry &table, DuckLakeSnapshot snapshot) : table(table), snapshot(snapshot) {
	}

	DuckLakeTableEntry &table;
	string table_name;
	vector<string> column_names;
	vector<LogicalType> column_types;
	DuckLakeSnapshot snapshot;
	idx_t table_id;
};

} // namespace duckdb
