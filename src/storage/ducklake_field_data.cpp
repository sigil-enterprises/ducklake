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

LogicalType GetStructType(const vector<unique_ptr<DuckLakeFieldId>> &new_children) {
	child_list_t<LogicalType> child_types;
	for(auto &child : new_children) {
		child_types.emplace_back(child->Name(), child->Type());
	}
	return LogicalType::STRUCT(std::move(child_types));
}

unique_ptr<DuckLakeFieldId> DuckLakeFieldId::AddField(const vector<string> &column_path, unique_ptr<DuckLakeFieldId> new_child, idx_t depth) const {
	vector<unique_ptr<DuckLakeFieldId>> new_children;
	if (depth >= column_path.size()) {
		// leaf - add the column at this level
		// copy over all of the other columns as-is
		for(auto &child : children) {
			new_children.push_back(child->Copy());
		}
		new_children.push_back(std::move(new_child));
	} else {
		// not the leaf - find the child to add it to and recurse
		bool found = false;
		for(idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
			auto &child = *children[child_idx];
			if (StringUtil::CIEquals(child.Name(), column_path[depth])) {
				// found it!
				auto new_field = child.AddField(column_path, std::move(new_child), depth + 1);
				new_children.push_back(std::move(new_field));
				found = true;
			} else {
				// this entry can be copied as-is
				new_children.push_back(child.Copy());
			}
		}
		if (!found) {
			throw InternalException("DuckLakeFieldId::AddField - child not found in struct path");
		}
	}
	auto new_type = GetStructType(new_children);
	return make_uniq<DuckLakeFieldId>(GetFieldIndex(), Name(), std::move(new_type), std::move(new_children));
}

unique_ptr<DuckLakeFieldId> DuckLakeFieldId::RemoveField(const vector<string> &column_path, idx_t depth) const {
	vector<unique_ptr<DuckLakeFieldId>> new_children;
	bool found = false;
	for(idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &child = *children[child_idx];
		if (StringUtil::CIEquals(child.Name(), column_path[depth])) {
			// found it!
			found = true;
			if (depth + 1 >= column_path.size()) {
				// leaf - remove the column at this level
				continue;
			} else {
				// not the leaf - find the child to drop it from and recurse
				new_children.push_back(child.RemoveField(column_path, depth + 1));
			}
		} else {
			// this entry can be copied as-is
			new_children.push_back(child.Copy());
		}
	}
	if (!found) {
		throw InternalException("DuckLakeFieldId::AddField - child not found in struct path");
	}
	auto new_type = GetStructType(new_children);
	return make_uniq<DuckLakeFieldId>(GetFieldIndex(), Name(), std::move(new_type), std::move(new_children));
}

unique_ptr<DuckLakeFieldId> DuckLakeFieldId::RenameField(const vector<string> &column_path, const string &new_name, idx_t depth) const {
	vector<unique_ptr<DuckLakeFieldId>> new_children;
	bool found = false;
	idx_t child_idx;
	for(child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &child = *children[child_idx];
		if (StringUtil::CIEquals(child.Name(), column_path[depth])) {
			// found it!
			found = true;
			if (depth + 1 >= column_path.size()) {
				// leaf - rename the column at this level
				auto copied_entry = child.Copy();
				auto renamed_entry = make_uniq<DuckLakeFieldId>(copied_entry->GetFieldIndex(), new_name, std::move(copied_entry->type), std::move(copied_entry->children));
				new_children.push_back(std::move(renamed_entry));
			} else {
				// not the leaf - find the child to rename it and recurse
				new_children.push_back(child.RenameField(column_path, new_name, depth + 1));
			}
		} else {
			// this entry can be copied as-is
			new_children.push_back(child.Copy());
		}
	}
	if (!found) {
		throw InternalException("DuckLakeFieldId::AddField - child not found in struct path");
	}
	auto new_type = GetStructType(new_children);
	return make_uniq<DuckLakeFieldId>(GetFieldIndex(), Name(), std::move(new_type), std::move(new_children));
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

optional_ptr<const DuckLakeFieldId> DuckLakeFieldData::GetByFieldIndex(FieldIndex id) const {
	auto entry = field_references.find(id);
	if (entry == field_references.end()) {
		return nullptr;
	}
	return entry->second.get();
}

optional_ptr<const DuckLakeFieldId> DuckLakeFieldId::GetChildByName(const string &child_name) const {
	auto entry = child_map.find(child_name);
	if (entry == child_map.end()) {
		return nullptr;
	}
	return *children[entry->second];
}

const DuckLakeFieldId &DuckLakeFieldId::GetChildByIndex(idx_t index) const {
	return *children[index];
}

optional_ptr<const DuckLakeFieldId> DuckLakeFieldData::GetByNames(PhysicalIndex id, const vector<string> &column_names) const {
	const_reference<DuckLakeFieldId> result = GetByRootIndex(id);
	for (idx_t i = 1; i < column_names.size(); ++i) {
		auto &current = result.get();
		auto next_child = current.GetChildByName(column_names[i]);
		if (!next_child) {
			return nullptr;
		}
		result = *next_child;
	}
	return result.get();
}

} // namespace duckdb
