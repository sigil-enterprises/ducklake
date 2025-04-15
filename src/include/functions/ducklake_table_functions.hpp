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
class DuckLakeCatalog;

struct MetadataBindData : public TableFunctionData {
	MetadataBindData() {
	}

	vector<vector<Value>> rows;
};

class BaseMetadataFunction : public TableFunction {
public:
	BaseMetadataFunction(string name, table_function_bind_t bind);

	static Catalog &GetCatalog(ClientContext &context, const Value &input);
};

class DuckLakeSnapshotsFunction : public BaseMetadataFunction {
public:
	DuckLakeSnapshotsFunction();
};

class DuckLakeTableInsertionsFunction : public TableFunction {
public:
	DuckLakeTableInsertionsFunction();
};

class DuckLakeTableDeletionsFunction : public TableFunction {
public:
	DuckLakeTableDeletionsFunction();
};

} // namespace duckdb
