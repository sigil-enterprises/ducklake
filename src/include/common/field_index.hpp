//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/field_index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {

struct FieldIndex {
	FieldIndex() : index(DConstants::INVALID_INDEX) {}
	explicit FieldIndex(idx_t index) : index(index) {
	}

	idx_t index;

	inline bool operator==(const FieldIndex &rhs) const {
		return index == rhs.index;
	};
	inline bool operator!=(const FieldIndex &rhs) const {
		return index != rhs.index;
	};
	inline bool operator<(const FieldIndex &rhs) const {
		return index < rhs.index;
	};
	bool IsValid() {
		return index != DConstants::INVALID_INDEX;
	}
};

} // namespace duckdb
