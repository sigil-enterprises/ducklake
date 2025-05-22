//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_initializer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_catalog.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
class DuckLakeTransaction;

class DuckLakeInitializer {
public:
	DuckLakeInitializer(ClientContext &context, DuckLakeCatalog &catalog, const string &metadata_database,
	                    const string &metadata_path, string &schema, string &data_path);

public:
	void Initialize();

private:
	void InitializeNewDuckLake(DuckLakeTransaction &transaction, bool has_explicit_schema);
	void LoadExistingDuckLake(DuckLakeTransaction &transaction);
	void InitializeDataPath();

private:
	ClientContext &context;
	DuckLakeCatalog &catalog;
	const string &metadata_database;
	const string &metadata_path;
	string &schema;
	string &data_path;
};

} // namespace duckdb
