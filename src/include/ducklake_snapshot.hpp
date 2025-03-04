//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_snapshot.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

struct DuckLakeSnapshot {
	DuckLakeSnapshot(idx_t snapshot_id, idx_t schema_version) : snapshot_id(snapshot_id), schema_version(schema_version) {}

	idx_t snapshot_id;
	idx_t schema_version;
};

} // namespace duckdb
