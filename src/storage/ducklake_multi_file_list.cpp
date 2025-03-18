#include "common/ducklake_util.hpp"
#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_multi_file_list.hpp"
#include "storage/ducklake_multi_file_reader.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "storage/ducklake_table_entry.hpp"

namespace duckdb {

DuckLakeMultiFileList::DuckLakeMultiFileList(DuckLakeTransaction &transaction, DuckLakeFunctionInfo &read_info,
                                             vector<DuckLakeDataFile> transaction_local_files_p, string filter_p)
    : MultiFileList(vector<string> {}, FileGlobOptions::ALLOW_EMPTY), transaction(transaction), read_info(read_info),
      read_file_list(false), transaction_local_files(std::move(transaction_local_files_p)),
      filter(std::move(filter_p)) {
}

unique_ptr<MultiFileList> DuckLakeMultiFileList::ComplexFilterPushdown(ClientContext &context,
                                                                       const MultiFileReaderOptions &options,
                                                                       MultiFilePushdownInfo &info,
                                                                       vector<unique_ptr<Expression>> &filters) {
	return nullptr;
}

string CastValueToTarget(const Value &val, const LogicalType &type) {
	if (type.IsNumeric()) {
		// for numerics we cast the min/max instead
		return val.ToString();
	}
	// convert to a string
	return DuckLakeUtil::SQLLiteralToString(val.ToString());
}

string CastStatsToTarget(const string &stats, const LogicalType &type) {
	// we only need to cast numerics
	if (type.IsNumeric()) {
		return stats + "::" + type.ToString();
	}
	return stats;
}

string GenerateFilterPushdown(const TableFilter &filter, unordered_set<string> &referenced_stats) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		auto &type = constant_filter.constant.type();
		switch (type.id()) {
		case LogicalTypeId::BLOB:
			return string();
		default:
			break;
		}

		auto constant_str = CastValueToTarget(constant_filter.constant, type);
		auto min_value = CastStatsToTarget("min_value", type);
		auto max_value = CastStatsToTarget("max_value", type);
		switch (constant_filter.comparison_type) {
		case ExpressionType::COMPARE_EQUAL:
			// x = constant
			// this can only be true if "constant BETWEEN min AND max"
			referenced_stats.insert("min_value");
			referenced_stats.insert("max_value");
			return StringUtil::Format("%s BETWEEN %s AND %s", constant_str, min_value, max_value);
		case ExpressionType::COMPARE_NOTEQUAL:
			// x <> constant
			// this can only be false if "constant = min AND constant = max" (i.e. min = max = constant)
			// skip this for now
			return string();
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			// x >= constant
			// this can only be true if "max >= C"
			referenced_stats.insert("max_value");
			return StringUtil::Format("%s >= %s", max_value, constant_str);
		case ExpressionType::COMPARE_GREATERTHAN:
			// x > constant
			// this can only be true if "max > C"
			referenced_stats.insert("max_value");
			return StringUtil::Format("%s > %s", max_value, constant_str);
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			// x <= constant
			// this can only be true if "min <= C"
			referenced_stats.insert("min_value");
			return StringUtil::Format("%s <= %s", min_value, constant_str);
		case ExpressionType::COMPARE_LESSTHAN:
			// x < constant
			// this can only be true if "min < C"
			referenced_stats.insert("min_value");
			return StringUtil::Format("%s < %s", min_value, constant_str);
		default:
			// unsupported
			return string();
		}
		break;
	}
	case TableFilterType::IS_NULL:
		// IS NULL can only be true if the file has any NULL values
		referenced_stats.insert("null_count");
		return "null_count > 0";
	case TableFilterType::IS_NOT_NULL:
		// IS NOT NULL can only be true if the file has any valid values
		referenced_stats.insert("value_count");
		return "value_count > 0";
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction_or_filter = filter.Cast<ConjunctionOrFilter>();
		string result;
		for (auto &child_filter : conjunction_or_filter.child_filters) {
			if (!result.empty()) {
				result += " OR ";
			}
			string child_str = GenerateFilterPushdown(*child_filter, referenced_stats);
			if (child_str.empty()) {
				return string();
			}
			result += child_str;
		}
		return result;
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_and_filter = filter.Cast<ConjunctionAndFilter>();
		string result;
		for (auto &child_filter : conjunction_and_filter.child_filters) {
			if (!result.empty()) {
				result += " AND ";
			}
			string child_str = GenerateFilterPushdown(*child_filter, referenced_stats);
			if (child_str.empty()) {
				return string();
			}
			result += child_str;
		}
		return result;
	}
	// FIXME: we probably want to support IN filters as well here
	default:
		// unsupported filter
		return string();
	}
}

unique_ptr<MultiFileList>
DuckLakeMultiFileList::DynamicFilterPushdown(ClientContext &context, const MultiFileReaderOptions &options,
                                             const vector<string> &names, const vector<LogicalType> &types,
                                             const vector<column_t> &column_ids, TableFilterSet &filters) const {
	string filter;
	for (auto &entry : filters.filters) {
		auto column_id = entry.first;
		// FIXME: handle structs
		auto column_index = PhysicalIndex(column_ids[column_id]);
		auto &root_id = read_info.table.GetFieldId(column_index);
		unordered_set<string> referenced_stats;
		auto new_filter = GenerateFilterPushdown(*entry.second, referenced_stats);
		if (new_filter.empty()) {
			// failed to generate filter for this column
			continue;
		}
		// generate the final filter for this column
		string final_filter;
		final_filter = "table_id=" + to_string(read_info.table_id.index);
		final_filter += " AND ";
		final_filter += "column_id=" + to_string(root_id.GetFieldIndex().index);
		final_filter += " AND ";
		final_filter += "(";
		// if any of the referenced stats are NULL we cannot prune
		for (auto &stats_name : referenced_stats) {
			final_filter += stats_name + " IS NULL OR ";
		}
		// finally add the filter
		final_filter += "(" + new_filter + "))";
		// add the filter to the list of filters
		if (!filter.empty()) {
			filter += " AND ";
		}
		filter += StringUtil::Format(
		    "data_file_id IN (SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_file_column_statistics WHERE %s)",
		    final_filter);
	}
	if (!filter.empty()) {
		return make_uniq<DuckLakeMultiFileList>(transaction, read_info, transaction_local_files, std::move(filter));
	}
	return nullptr;
}

vector<string> DuckLakeMultiFileList::GetAllFiles() {
	return GetFiles();
}

FileExpandResult DuckLakeMultiFileList::GetExpandResult() {
	return FileExpandResult::MULTIPLE_FILES;
}

idx_t DuckLakeMultiFileList::GetTotalFileCount() {
	return GetFiles().size();
}

unique_ptr<NodeStatistics> DuckLakeMultiFileList::GetCardinality(ClientContext &context) {
	auto stats = read_info.table.GetTableStats(context);
	if (!stats) {
		return nullptr;
	}
	return make_uniq<NodeStatistics>(stats->record_count);
}

DuckLakeTableEntry &DuckLakeMultiFileList::GetTable() {
	return read_info.table;
}

string DuckLakeMultiFileList::GetFile(idx_t i) {
	auto &files = GetFiles();
	if (i < files.size()) {
		return files[i];
	}
	return string();
}

const vector<string> &DuckLakeMultiFileList::GetFiles() {
	lock_guard<mutex> l(file_lock);
	if (!read_file_list) {
		// we have not read the file list yet - read it
		if (!read_info.table_id.IsTransactionLocal()) {
			// not a transaction local table - read the file list from the metadata store
			auto query = StringUtil::Format(R"(
SELECT path
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE table_id=%d AND {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
		)",
			                                read_info.table_id.index);
			if (!filter.empty()) {
				query += "\nAND " + filter;
			}

			auto result = transaction.Query(read_info.snapshot, query);
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to get data file list from DuckLake: ");
			}

			for (auto &row : *result) {
				files.push_back(row.GetValue<string>(0));
			}
		}
		for (auto &transaction_local_file : transaction_local_files) {
			files.push_back(transaction_local_file.file_name);
		}
		read_file_list = true;
	}
	return files;
}

} // namespace duckdb