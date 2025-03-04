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
#include "ducklake_snapshot.hpp"

namespace duckdb {
class DuckLakeTransaction;

using ducklake_entries_map_t = case_insensitive_map_t<unique_ptr<CatalogEntry>>;

//! The DuckLakeCatalogSet contains a set of catalog entries for a given schema version of the DuckLake
//! Note that we don't need any locks to access this - the catalog set is constant for a given snapshot
class DuckLakeCatalogSet {
public:
	DuckLakeCatalogSet(CatalogType catalog_type, string schema_name_p) :
		catalog_type(catalog_type), schema_name(std::move(schema_name_p)) {}
	DuckLakeCatalogSet(CatalogType catalog_type, ducklake_entries_map_t catalog_entries_p) :
		catalog_type(catalog_type), catalog_entries(std::move(catalog_entries_p)) {}

	void CreateEntry(unique_ptr<CatalogEntry> entry);
	optional_ptr<CatalogEntry> GetEntry(DuckLakeTransaction &transaction, const string &name);

	template<class T>
	optional_ptr<T> GetEntry(DuckLakeTransaction &transaction, const string &name) {
		auto entry = GetEntry(transaction, name);
		if (!entry) {
			return nullptr;
		}
		return entry->Cast<T>();
	}

	const ducklake_entries_map_t &GetEntries() {
		return catalog_entries;
	}

private:
	CatalogType catalog_type;
	string schema_name;
	ducklake_entries_map_t catalog_entries;
};

} // namespace duckdb
