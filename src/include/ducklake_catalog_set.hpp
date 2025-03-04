//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_catalog_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/catalog/catalog_entry.hpp"

namespace duckdb {

class DuckLakeCatalogSet {
public:
	void CreateEntry(unique_ptr<CatalogEntry> entry);
	optional_ptr<CatalogEntry> GetEntry();

private:
	mutex lock;
	case_insensitive_map_t<unique_ptr<CatalogEntry>> catalog_entries;
};

} // namespace duckdb
