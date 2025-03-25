#include "storage/ducklake_field_data.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {

void DuckLakeFieldData::Add(unique_ptr<DuckLakeFieldId> field_info) {
	// add field references' to the map
	vector<const_reference<DuckLakeFieldId>> active_fields {*field_info};
	for (idx_t i = 0; i < active_fields.size(); i++) {
		auto &current_field = active_fields[i].get();
		for (auto &child_field : current_field.Children()) {
			active_fields.push_back(const_reference<DuckLakeFieldId>(*child_field));
		}
		field_references.insert(
		    make_pair(current_field.GetFieldIndex(), const_reference<DuckLakeFieldId>(current_field)));
	}

	field_ids.push_back(std::move(field_info));
}

DuckLakeFieldId::DuckLakeFieldId(FieldIndex index, string name_p, LogicalType type_p)
    : id(index), name(std::move(name_p)), type(std::move(type_p)) {
}

DuckLakeFieldId::DuckLakeFieldId(FieldIndex index, string name_p, LogicalType type_p,
                                 vector<unique_ptr<DuckLakeFieldId>> children_p)
    : id(index), name(std::move(name_p)), type(std::move(type_p)), children(std::move(children_p)) {
	for (idx_t child_idx = 0; child_idx < children.size(); ++child_idx) {
		auto &child = children[child_idx];
		auto entry = child_map.find(child->name);
		if (entry != child_map.end()) {
			throw InvalidInputException("Duplicate child name \"%s\" found in column \"%s\"", child->name, name);
		}
		child_map.insert(make_pair(child->name, child_idx));
	}
}
unique_ptr<DuckLakeFieldId> DuckLakeFieldId::FieldIdFromType(const string &name, const LogicalType &type,
                                                             idx_t &column_id) {
	auto field_index = FieldIndex(column_id++);
	vector<unique_ptr<DuckLakeFieldId>> field_children;
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		for (auto &entry : StructType::GetChildTypes(type)) {
			field_children.push_back(FieldIdFromType(entry.first, entry.second, column_id));
		}
		break;
	}
	case LogicalTypeId::LIST:
		field_children.push_back(FieldIdFromType("element", ListType::GetChildType(type), column_id));
		break;
	case LogicalTypeId::ARRAY:
		field_children.push_back(FieldIdFromType("element", ArrayType::GetChildType(type), column_id));
		break;
	case LogicalTypeId::MAP:
		field_children.push_back(FieldIdFromType("key", MapType::KeyType(type), column_id));
		field_children.push_back(FieldIdFromType("value", MapType::ValueType(type), column_id));
		break;
	default:
		break;
	}
	return make_uniq<DuckLakeFieldId>(field_index, name, type, std::move(field_children));
}

shared_ptr<DuckLakeFieldData> DuckLakeFieldData::FromColumns(const ColumnList &columns) {
	// generate field ids based on the column ids
	idx_t column_id = 1;
	return FromColumns(columns, column_id);
}

shared_ptr<DuckLakeFieldData> DuckLakeFieldData::FromColumns(const ColumnList &columns, idx_t &column_id) {
	auto field_data = make_shared_ptr<DuckLakeFieldData>();
	for (auto &col : columns.Logical()) {
		auto field_id = DuckLakeFieldId::FieldIdFromType(col.Name(), col.Type(), column_id);
		field_data->Add(std::move(field_id));
	}
	return field_data;
}

unique_ptr<DuckLakeFieldId> DuckLakeFieldId::Copy() const {
	vector<unique_ptr<DuckLakeFieldId>> new_children;
	for (auto &child : children) {
		new_children.push_back(child->Copy());
	}
	return make_uniq<DuckLakeFieldId>(id, name, type, std::move(new_children));
}

unique_ptr<DuckLakeFieldId> DuckLakeFieldId::Rename(const DuckLakeFieldId &field_id, const string &new_name) {
	auto result = field_id.Copy();
	result->name = new_name;
	return result;
}

shared_ptr<DuckLakeFieldData> DuckLakeFieldData::RenameColumn(const DuckLakeFieldData &field_data,
                                                              FieldIndex rename_index, const string &new_name) {
	auto result = make_shared_ptr<DuckLakeFieldData>();
	for (auto &existing_id : field_data.field_ids) {
		unique_ptr<DuckLakeFieldId> field_id;
		if (existing_id->GetFieldIndex() == rename_index) {
			field_id = DuckLakeFieldId::Rename(*existing_id, new_name);
		} else {
			field_id = existing_id->Copy();
		}
		result->Add(std::move(field_id));
	}
	return result;
}

shared_ptr<DuckLakeFieldData> DuckLakeFieldData::AddColumn(const DuckLakeFieldData &field_data,
                                                           const ColumnDefinition &new_col, idx_t &next_column_id) {
	auto result = make_shared_ptr<DuckLakeFieldData>();
	for (auto &existing_id : field_data.field_ids) {
		result->Add(existing_id->Copy());
	}
	auto field_id = DuckLakeFieldId::FieldIdFromType(new_col.Name(), new_col.Type(), next_column_id);
	result->Add(std::move(field_id));
	return result;
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
	for (idx_t i = 1; i < column_names.size(); ++i) {
		auto &current = result.get();
		result = current.GetChildByName(column_names[i]);
	}
	return result.get();
}

} // namespace duckdb
