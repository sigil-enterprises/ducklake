//===----------------------------------------------------------------------===//
//                         DuckDB
//
// ducklake_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class DuckLakeUtil {
public:
	static string ParseQuotedValue(const string &input, idx_t &pos);
	static vector<string> ParseQuotedList(const string &input, char list_separator = ',');
	static string SQLIdentifierToString(const string &text);
	static string SQLLiteralToString(const string &text);
};

} // namespace duckdb
