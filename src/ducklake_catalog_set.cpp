#include "ducklake_catalog_set.hpp"

namespace duckdb {

void DuckLakeCatalogSet::CreateEntry(unique_ptr<CatalogEntry> catalog_entry) {
	auto &entry_name = catalog_entry->name;
	auto entry = catalog_entries.find(entry_name);
	if (entry != catalog_entries.end()) {
		// entry already exists
		//! FIXME: check that it exists for this transaction
		throw CatalogException("Entry %s already exists", entry_name);
	}
	catalog_entries.insert(make_pair(entry_name, std::move(catalog_entry)));
}

optional_ptr<CatalogEntry> DuckLakeCatalogSet::GetEntry() {
	throw InternalException("GetEntry");
}

} // namespace duckdb
