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
#include "ducklake_transaction.hpp"
#include "ducklake_snapshot.hpp"

namespace duckdb {

using ducklake_entries_map_t = case_insensitive_map_t<unique_ptr<CatalogEntry>>;

//! The DuckLakeCatalogSet contains a set of catalog entries for a given schema version of the DuckLake
//! Note that we don't need any locks to access this - the catalog set is constant for a given snapshot
class DuckLakeCatalogSet {
public:
	explicit DuckLakeCatalogSet(ducklake_entries_map_t catalog_entries_p) :
		catalog_entries(std::move(catalog_entries_p)) {}

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
	ducklake_entries_map_t catalog_entries;
};

} // namespace duckdb
