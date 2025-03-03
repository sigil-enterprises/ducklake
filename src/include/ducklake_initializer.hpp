//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_initializer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "ducklake_catalog.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

class DuckLakeInitializer {
public:
	DuckLakeInitializer(ClientContext &context, AttachedDatabase &metadata_database, const string &schema, const string &data_path);

public:
	void Initialize();

private:
	void InitializeNewDuckLake();
	void LoadExistingDuckLake();
	TableCatalogEntry &CreateTable(string name, vector<string> column_names, vector<LogicalType> column_types);
	void Insert(TableCatalogEntry &table, DataChunk &data);

private:
	ClientContext &context;
	AttachedDatabase &metadata_database;
	const string &schema;
	const string &data_path;
};

} // namespace duckdb
