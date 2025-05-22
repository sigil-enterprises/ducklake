//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "common/ducklake_encryption.hpp"

namespace duckdb {

struct DuckLakeOptions {
	string metadata_database;
	string metadata_path;
	string metadata_schema;
	string data_path;
	AccessMode access_mode = AccessMode::AUTOMATIC;
	DuckLakeEncryption encryption = DuckLakeEncryption::AUTOMATIC;
	idx_t data_inlining_row_limit = 0;
};

} // namespace duckdb
