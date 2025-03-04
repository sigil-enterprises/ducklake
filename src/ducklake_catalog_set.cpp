#include "ducklake_catalog_set.hpp"
#include "ducklake_transaction.hpp"
namespace duckdb {

optional_ptr<CatalogEntry> DuckLakeCatalogSet::GetEntry(DuckLakeTransaction &transaction, const string &name) {
	//! FIXME: search in transaction local storage
	auto entry = catalog_entries.find(name);
	if (entry == catalog_entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

} // namespace duckdb
