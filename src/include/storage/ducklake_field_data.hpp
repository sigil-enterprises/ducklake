//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_field_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "common/index.hpp"

namespace duckdb {
struct AlterTableInfo;
struct SetPartitionedByInfo;
class DuckLakeTransaction;

class DuckLakeFieldId {
public:
	DuckLakeFieldId(FieldIndex index, string name, LogicalType type);
	DuckLakeFieldId(FieldIndex index, string name, LogicalType type, vector<unique_ptr<DuckLakeFieldId>> children);

	FieldIndex GetFieldIndex() const {
		return id;
	}
	const string &Name() const {
		return name;
	}
	const LogicalType &Type() const {
		return type;
	}
	const vector<unique_ptr<DuckLakeFieldId>> &Children() const {
		return children;
	}
	const DuckLakeFieldId &GetChildByIndex(idx_t index) const;
	const DuckLakeFieldId &GetChildByName(const string &name) const;

private:
	FieldIndex id;
	string name;
	LogicalType type;
	vector<unique_ptr<DuckLakeFieldId>> children;
	case_insensitive_map_t<idx_t> child_map;
};

class DuckLakeFieldData {
public:
	DuckLakeFieldData() = default;
	// disable copy constructors
	DuckLakeFieldData(const DuckLakeFieldData &other) = delete;
	DuckLakeFieldData &operator=(const DuckLakeFieldData &) = delete;
	//! enable move constructors
	DuckLakeFieldData(DuckLakeFieldData &&other) noexcept = default;
	DuckLakeFieldData &operator=(DuckLakeFieldData &&) noexcept = default;

public:
	void Add(unique_ptr<DuckLakeFieldId> field_info);
	const DuckLakeFieldId &GetByRootIndex(PhysicalIndex id) const;
	const DuckLakeFieldId &GetByFieldIndex(FieldIndex id) const;
	const DuckLakeFieldId &GetByNames(PhysicalIndex id, const vector<string> &column_names) const;
	idx_t GetColumnCount() {
		return field_ids.size();
	}

private:
	vector<unique_ptr<DuckLakeFieldId>> field_ids;
	map<FieldIndex, const_reference<DuckLakeFieldId>> field_references;
};

} // namespace duckdb
