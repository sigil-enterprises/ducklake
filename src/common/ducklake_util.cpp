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

vector<ParsedTableInfo> DuckLakeUtil::ParseTableList(const string &input) {
	vector<ParsedTableInfo> result;
	if (input.empty()) {
		return result;
	}
	idx_t pos = 0;
	while (true) {
		ParsedTableInfo table_data;
		table_data.schema = DuckLakeUtil::ParseQuotedValue(input, pos);
		if (pos >= input.size() || input[pos] != '.') {
			throw InvalidInputException("Failed to parse table list - expected a dot");
		}
		pos++;
		table_data.table = DuckLakeUtil::ParseQuotedValue(input, pos);
		result.push_back(std::move(table_data));
		if (pos >= input.size()) {
			break;
		}
		if (input[pos] != ',') {
			throw InvalidInputException("Failed to parse table list - expected a comma");
		}
		pos++;
	}
	return result;
}

unordered_set<idx_t> DuckLakeUtil::ParseDropList(const string &input) {
	unordered_set<idx_t> result;
	if (input.empty()) {
		return result;
	}
	auto splits = StringUtil::Split(input, ",");
	for (auto &split : splits) {
		result.insert(std::stoull(split));
	}
	return result;
}

string DuckLakeUtil::SQLIdentifierToString(const string &text) {
	return "\"" + StringUtil::Replace(text, "\"", "\"\"") + "\"";
}

string DuckLakeUtil::SQLLiteralToString(const string &text) {
	return "'" + StringUtil::Replace(text, "'", "''") + "'";
}

} // namespace duckdb
