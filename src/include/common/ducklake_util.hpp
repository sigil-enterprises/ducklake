//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {

struct ParsedTableInfo {
	string schema;
	string table;
};

class DuckLakeUtil {
public:
	static string ParseQuotedValue(const string &input, idx_t &pos);
	static vector<string> ParseQuotedList(const string &input, char list_separator = ',');
	static string SQLIdentifierToString(const string &text);
	static string SQLLiteralToString(const string &text);

	static vector<ParsedTableInfo> ParseTableList(const string &input);
	static unordered_set<idx_t> ParseDropList(const string &input);
};

} // namespace duckdb
