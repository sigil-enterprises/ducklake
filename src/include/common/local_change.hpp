//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/local_change.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "common/index.hpp"

namespace duckdb {

enum class LocalChangeType { NONE, CREATED, RENAMED, SET_PARTITION_KEY, SET_COMMENT, SET_COLUMN_COMMENT };

struct LocalChange {
	LocalChange(LocalChangeType type) // NOLINT: allow implicit conversion from LocalChangeType
	    : type(type) {
	}

	LocalChangeType type;
	//! For SET COLUMN COMMENT
	FieldIndex field_index;

	static LocalChange SetColumnComment(FieldIndex field_idx) {
		LocalChange result(LocalChangeType::SET_COLUMN_COMMENT);
		result.field_index = field_idx;
		return result;
	}
};

} // namespace duckdb
