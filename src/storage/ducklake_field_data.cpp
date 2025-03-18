#include "storage/ducklake_field_data.hpp"

namespace duckdb {

void DuckLakeFieldData::Add(unique_ptr<DuckLakeFieldId> field_info) {
	// add field references' to the map
	vector<const_reference<DuckLakeFieldId>> active_fields { *field_info };
	for(idx_t i = 0; i < active_fields.size(); i++) {
		auto &current_field = active_fields[i].get();
		for(auto &child_field : current_field.Children()) {
			active_fields.push_back(const_reference<DuckLakeFieldId>(*child_field));
		}
		field_references.insert(make_pair(current_field.GetFieldIndex(), const_reference<DuckLakeFieldId>(current_field)));
	}

	field_ids.push_back(std::move(field_info));
}

DuckLakeFieldId::DuckLakeFieldId(FieldIndex index, string name_p, LogicalType type_p) : id(index), name(std::move(name_p)), type(std::move(type_p)) {
}

DuckLakeFieldId::DuckLakeFieldId(FieldIndex index, string name_p, LogicalType type_p, vector<unique_ptr<DuckLakeFieldId>> children_p) : id(index), name(std::move(name_p)), type(std::move(type_p)), children(std::move(children_p)) {
	for(idx_t child_idx = 0; child_idx < children.size(); ++child_idx) {
		auto &child = children[child_idx];
		auto entry = child_map.find(child->name);
		if (entry != child_map.end()) {
			throw InvalidInputException("Duplicate child name \"%s\" found in column \"%s\"", child->name, name);
		}
		child_map.insert(make_pair(child->name, child_idx));
	}
}

const DuckLakeFieldId &DuckLakeFieldData::GetByRootIndex(PhysicalIndex id) const {
	return *field_ids[id.index];
}

const DuckLakeFieldId &DuckLakeFieldData::GetByFieldIndex(FieldIndex id) const {
	auto entry = field_references.find(id);
	if (entry == field_references.end()) {
		throw InvalidInputException("Column with field id %d not found", id.index);
	}
	return entry->second;
}

const DuckLakeFieldId &DuckLakeFieldId::GetChildByName(const string &child_name) const {
	auto entry = child_map.find(child_name);
	if (entry == child_map.end()) {
		throw InvalidInputException("Sub-column \"%s\" not found in column \"%s\"", child_name, name);
	}
	return *children[entry->second];
}

const DuckLakeFieldId &DuckLakeFieldId::GetChildByIndex(idx_t index) const {
	return *children[index];
}

const DuckLakeFieldId &DuckLakeFieldData::GetByNames(PhysicalIndex id, const vector<string> &column_names) const {
	const_reference<DuckLakeFieldId> result = GetByRootIndex(id);
	for(idx_t i = 1; i < column_names.size(); ++i) {
		auto &current = result.get();
		result = current.GetChildByName(column_names[i]);
	}
	return result.get();
}

}
