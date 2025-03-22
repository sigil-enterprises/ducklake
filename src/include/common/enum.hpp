//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/enum.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

enum class TransactionLocalChange { NONE, CREATED, RENAMED, SET_PARTITION_KEY };

} // namespace duckdb
