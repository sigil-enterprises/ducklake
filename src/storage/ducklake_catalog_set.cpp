#include "storage/ducklake_catalog_set.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

DuckLakeCatalogSet::DuckLakeCatalogSet() {
}
DuckLakeCatalogSet::DuckLakeCatalogSet(ducklake_entries_map_t catalog_entries_p) : catalog_entries(std::move(catalog_entries_p)) {
}

void DuckLakeCatalogSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	auto name = entry->name;
	catalog_entries.insert(make_pair(std::move(name), std::move(entry)));
}

unique_ptr<CatalogEntry> DuckLakeCatalogSet::DropEntry(const string &name) {
	auto entry = catalog_entries.find(name);
	auto catalog_entry = std::move(entry->second);
	catalog_entries.erase(entry);
	return catalog_entry;
}

optional_ptr<CatalogEntry> DuckLakeCatalogSet::GetEntry(const string &name) {
	auto entry = catalog_entries.find(name);
	if (entry == catalog_entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

optional_ptr<CatalogEntry> DuckLakeCatalogSet::GetEntryById(idx_t index) {
	auto entry = id_to_entry_map.find(index);
	if (entry == id_to_entry_map.end()) {
		return nullptr;
	}
	return entry->second.get();
}

void DuckLakeCatalogSet::AddEntry(DuckLakeSchemaEntry &schema, idx_t id, unique_ptr<CatalogEntry> entry) {
	auto catalog_type = entry->type;
	id_to_entry_map.insert(make_pair(id, reference<CatalogEntry>(*entry)));
	schema.AddEntry(catalog_type, std::move(entry));
}

} // namespace duckdb
