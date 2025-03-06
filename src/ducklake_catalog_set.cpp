#include "ducklake_catalog_set.hpp"
#include "ducklake_transaction.hpp"

namespace duckdb {

void DuckLakeCatalogSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	auto name = entry->name;
	catalog_entries.insert(make_pair(std::move(name), std::move(entry)));
}

optional_ptr<CatalogEntry> DuckLakeCatalogSet::GetEntry(const string &name) {
	auto entry = catalog_entries.find(name);
	if (entry == catalog_entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

} // namespace duckdb
