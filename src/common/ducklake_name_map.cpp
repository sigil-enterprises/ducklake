#include "common/ducklake_name_map.hpp"

namespace duckdb {


hash_t DuckLakeNameMapEntry::GetHash() const {
	hash_t result = Hash(source_name.c_str(), source_name.size());
	for(auto &entry : child_entries) {
		result ^= entry.GetHash();
	}
	return result;
}
bool DuckLakeNameMapEntry::IsCompatibleWith(const DuckLakeNameMapEntry &other) const {
	if (source_name != other.source_name) {
		return false;
	}
	if (child_entries.size() != other.child_entries.size()) {
		return false;
	}
	// FIXME:
	throw InternalException("FIXME: check child entries");
}

hash_t DuckLakeNameMap::GetHash() const {
	hash_t result = Hash(table_id.index);
	for(auto &entry : column_maps) {
		result ^= entry.GetHash();
	}
	return result;
}

bool DuckLakeNameMap::IsCompatibleWith(const DuckLakeNameMap &other) const {
	if (table_id.index != other.table_id.index) {
		return false;
	}
	throw InternalException("FIXME: check child entries");
	return true;
}

MappingIndex DuckLakeNameMapSet::TryGetCompatibleNameMap(const DuckLakeNameMap &name_map) {
	// FIXME: try to find a compatible set
	return MappingIndex();
}

void DuckLakeNameMapSet::Add(DuckLakeNameMap name_map) {
	auto mapping_id = name_map.id;
	auto mapping = make_uniq<DuckLakeNameMap>(name_map);
	name_maps.emplace(mapping_id, std::move(mapping));
}

}
