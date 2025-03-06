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
	DuckLakeSnapshot(idx_t snapshot_id, idx_t schema_version, idx_t next_catalog_id, idx_t next_file_id)
	    : snapshot_id(snapshot_id), schema_version(schema_version),
	      next_catalog_id(next_catalog_id), next_file_id(next_file_id) {
	}

	idx_t snapshot_id;
	idx_t schema_version;
	idx_t next_catalog_id;
	idx_t next_schema_id;
	idx_t next_file_id;
};

} // namespace duckdb
