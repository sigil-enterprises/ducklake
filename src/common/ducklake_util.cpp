#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

string DuckLakeUtil::ParseQuotedValue(const string &input, idx_t &pos) {
	if (pos >= input.size() || input[pos] != '"') {
		throw InvalidInputException("Failed to parse quoted value - expected a quote");
	}
	string result;
	pos++;
	for (; pos < input.size(); pos++) {
		if (input[pos] == '"') {
			pos++;
			// check if this is an escaped quote
			if (pos < input.size() && input[pos] == '"') {
				// escaped quote
				result += '"';
				continue;
			}
			return result;
		}
		result += input[pos];
	}
	throw InvalidInputException("Failed to parse quoted value - unterminated quote");
}

vector<string> DuckLakeUtil::ParseQuotedList(const string &input, char list_separator) {
	vector<string> result;
	if (input.empty()) {
		return result;
	}
	idx_t pos = 0;
	while (true) {
		result.push_back(ParseQuotedValue(input, pos));
		if (pos >= input.size()) {
			break;
		}
		if (input[pos] != list_separator) {
			throw InvalidInputException("Failed to parse list - expected a %s", string(1, list_separator));
		}
		pos++;
	}
	return result;
}

ParsedCatalogEntry DuckLakeUtil::ParseCatalogEntry(const string &input) {
	ParsedCatalogEntry result_data;
	idx_t pos = 0;
	result_data.schema = DuckLakeUtil::ParseQuotedValue(input, pos);
	if (pos >= input.size() || input[pos] != '.') {
		throw InvalidInputException("Failed to parse catalog entry - expected a dot");
	}
	pos++;
	result_data.name = DuckLakeUtil::ParseQuotedValue(input, pos);
	if (pos < input.size()) {
		throw InvalidInputException("Failed to parse catalog entry - trailing data after quoted value");
	}
	return result_data;
}

string DuckLakeUtil::SQLIdentifierToString(const string &text) {
	return "\"" + StringUtil::Replace(text, "\"", "\"\"") + "\"";
}

string DuckLakeUtil::SQLLiteralToString(const string &text) {
	return "'" + StringUtil::Replace(text, "'", "''") + "'";
}

} // namespace duckdb
