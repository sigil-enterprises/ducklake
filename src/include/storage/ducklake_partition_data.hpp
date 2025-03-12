//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_partition_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"

namespace duckdb {
class BaseStatistics;

enum class DuckLakeTransformType {
	IDENTITY,
};

struct DuckLakeTransform {
	DuckLakeTransformType type;
};

struct DuckLakePartitionField {
	idx_t partition_key_index;
	idx_t column_id;
	DuckLakeTransform transform;
};

struct DuckLakePartition {
	idx_t partition_id;
	vector<DuckLakePartitionField> fields;
};

} // namespace duckdb
