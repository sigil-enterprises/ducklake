//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "common/ducklake_snapshot.hpp"
#include "common/index.hpp"

namespace duckdb {
class DuckLakeMultiFileList;
class DuckLakeTableEntry;

class DuckLakeFunctions {
public:
	//! Table Functions
	static TableFunction GetDuckLakeScanFunction(DatabaseInstance &instance);

	static unique_ptr<FunctionData> BindDuckLakeScan(ClientContext &context, TableFunction &function);
};

struct DuckLakeFunctionInfo : public TableFunctionInfo {
	DuckLakeFunctionInfo(DuckLakeTableEntry &table, DuckLakeSnapshot snapshot) : table(table), snapshot(snapshot) {
	}

	DuckLakeTableEntry &table;
	string table_name;
	vector<string> column_names;
	vector<LogicalType> column_types;
	DuckLakeSnapshot snapshot;
	TableIndex table_id;
};

} // namespace duckdb
