//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_types.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
class ClientContext;
class Vector;

class DuckLakeTypes {
public:
	static LogicalType FromString(const string &str);
	static string ToString(const LogicalType &str);
	static void CheckSupportedType(const LogicalType &type);

	static bool RequiresCast(const LogicalType &type);
	static bool RequiresCast(const vector<LogicalType> &types);
	//! If this type requires a cast, return the type to cast to
	static LogicalType GetCastedType(const LogicalType &type);

	//! Cast a vector holding data written under a prior (evolved) column type to the current column type.
	//! Unlike a plain VectorOperations::Cast, this tolerates STRUCTs (at any nesting depth, including inside
	//! LISTs) that have zero overlapping field names between the source and target - which can legitimately
	//! happen after a sequence of ALTER COLUMN ... SET DATA TYPE STRUCT(...) evolutions (e.g. every original
	//! field dropped). Missing target fields are filled with NULL, matching fields are cast recursively.
	static void CastEvolvedVector(ClientContext &context, Vector &source, Vector &target, idx_t count);
};

} // namespace duckdb
