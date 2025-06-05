#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_table_entry.hpp"

namespace duckdb {

struct DuckLakeAddDataFilesData : public TableFunctionData {
	DuckLakeAddDataFilesData(Catalog &catalog, DuckLakeTableEntry &table)
		: catalog(catalog), table(table) {
	}

	Catalog &catalog;
	DuckLakeTableEntry &table;
	vector<string> globs;
};

static unique_ptr<FunctionData> DuckLakeAddDataFilesBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &transaction = DuckLakeTransaction::Get(context, catalog);

	string schema_name;
	if (input.inputs[1].IsNull()) {
		throw InvalidInputException("Table name cannot be NULL");
	}
	auto table_name = StringValue::Get(input.inputs[1]);
	auto entry = catalog.GetEntry<TableCatalogEntry>(context, schema_name, table_name, OnEntryNotFound::THROW_EXCEPTION);
	auto &table = entry->Cast<DuckLakeTableEntry>();

	auto result = make_uniq<DuckLakeAddDataFilesData>(catalog, table);
	if (input.inputs[2].IsNull()) {
		throw InvalidInputException("File list cannot be NULL");
	}
	result->globs.push_back(StringValue::Get(input.inputs[2]));

	names.emplace_back("filename");
	return_types.emplace_back(LogicalType::VARCHAR);
	return result;
}

struct DuckLakeAddDataFilesState : public GlobalTableFunctionState {
	DuckLakeAddDataFilesState() {
	}

	bool finished = false;
};

unique_ptr<GlobalTableFunctionState> DuckLakeAddDataFilesInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DuckLakeAddDataFilesState>();
}

struct ParquetColumnStats {
	vector<Value> column_stats;
};

struct ParquetColumn {
	idx_t column_id;
	string name;
	string type;
	string converted_type;
	optional_idx scale;
	optional_idx precision;
	optional_idx field_id;
	string logical_type;
	vector<ParquetColumnStats> column_stats;

	vector<unique_ptr<ParquetColumn>> child_columns;
};

struct ParquetFileMetadata {
	string filename;
	vector<unique_ptr<ParquetColumn>> columns;
	unordered_map<idx_t, reference<ParquetColumn>> column_id_map;
};

Value GetStatsValue(string name, Value val) {
	child_list_t<Value> children;
	children.emplace_back("key", Value(name));
	children.emplace_back("value", std::move(val));
	return Value::STRUCT(std::move(children));
}

void DuckLakeAddDataFilesExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeAddDataFilesState>();
	auto &bind_data = data_p.bind_data->Cast<DuckLakeAddDataFilesData>();
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	// fetch the metadata, stats and columns from the various files
	unordered_map<string, unique_ptr<ParquetFileMetadata>> parquet_files;
	for(auto &glob : bind_data.globs) {
		// query the parquet_schema to figure out the schema for each of the columns
		auto result = transaction.Query(StringUtil::Format(R"(
SELECT file_name, name, type, num_children, converted_type, scale, precision, field_id, logical_type
FROM parquet_schema(%s)
)", SQLString(glob)));

		unique_ptr<ParquetFileMetadata> file;
		vector<reference<ParquetColumn>> current_column;
		vector<idx_t> child_counts;
		idx_t column_id = 0;
		for(auto &row : *result) {
			auto filename = row.GetValue<string>(0);
			auto child_count = row.IsNull(3) ? 0 : row.GetValue<idx_t>(3);
			if (!file || file->filename != filename) {
				if (!child_counts.empty()) {
					throw InvalidInputException("child_counts provided by parquet_schema are unaligned");
				}
				if (file) {
					auto &filename = file->filename;
					parquet_files.emplace(filename, std::move(file));
				}
				file = make_uniq<ParquetFileMetadata>();
				file->filename = filename;
				child_counts.push_back(child_count);
				column_id = 0;
				continue;
			}
			if (child_counts.empty()) {
				throw InvalidInputException("child_counts provided by parquet_schema are unaligned");
			}
			auto column = make_uniq<ParquetColumn>();
			column->column_id = column_id++;
			column->name = row.GetValue<string>(1);
			column->type = row.GetValue<string>(2);
			if (!row.IsNull(4)) {
				column->converted_type = row.GetValue<string>(4);
			}
			if (!row.IsNull(5)) {
				column->scale = row.GetValue<idx_t>(5);
			}
			if (!row.IsNull(6)) {
				column->precision = row.GetValue<idx_t>(6);
			}
			if (!row.IsNull(7)) {
				column->field_id = row.GetValue<idx_t>(7);
			}
			if (!row.IsNull(8)) {
				column->logical_type = row.GetValue<string>(8);
			}
			auto &column_ref = *column;

			if (current_column.empty()) {
				// add to root
				file->columns.push_back(std::move(column));
			} else {
				// add as child to last column
				current_column.back().get().child_columns.push_back(std::move(column));
			}
			// add to column id map
			file->column_id_map.emplace(column_ref.column_id, column_ref);

			// reduce the child count by one
			child_counts.back()--;
			if (child_counts.back() == 0) {
				// we exhausted all children at this layer - pop back child counts
				if (!current_column.empty()) {
					current_column.pop_back();
				}
				child_counts.pop_back();
			}
			if (child_count > 0) {
				// nested column: push back the child count and the current column and start reading children for this column
				current_column.push_back(column_ref);
				child_counts.push_back(child_count);
			}
		}
		if (file) {
			auto &filename = file->filename;
			parquet_files.emplace(filename, std::move(file));
		}
		// query the parquet_metadata to get the stats for each of the columns
		result = transaction.Query(StringUtil::Format(R"(
SELECT file_name, column_id, num_values, stats_min, stats_min_value, stats_max, stats_max_value, stats_null_count, total_compressed_size
FROM parquet_metadata(%s)
)", SQLString(glob)));
		for(auto &row : *result) {
			auto filename = row.GetValue<string>(0);
			auto entry = parquet_files.find(filename);
			if (entry == parquet_files.end()) {
				throw InvalidInputException("Parquet file was returned by parquet_metadata, but not returned by parquet_schema - did a Parquet file get added to a glob while processing?");
			}
			auto &parquet_file = entry->second;
			auto column_id = row.GetValue<idx_t>(1);
			// find the column this belongs to
			auto column_entry = parquet_file->column_id_map.find(column_id);
			if (column_entry == parquet_file->column_id_map.end()) {
				throw InvalidInputException("Column id not found in Parquet map?");
			}
			auto &column = column_entry->second.get();
			ParquetColumnStats stats;

			if (!row.IsNull(2)) {
				stats.column_stats.push_back(GetStatsValue("value_count", row.GetValue<string>(2)));
			}
			if (!row.IsNull(3)) {
				stats.column_stats.push_back(GetStatsValue("min", row.GetValue<string>(3)));
			} else if (!row.IsNull(4)) {
				stats.column_stats.push_back(GetStatsValue("min", row.GetValue<string>(4)));
			}
			if (!row.IsNull(5)) {
				stats.column_stats.push_back(GetStatsValue("max", row.GetValue<string>(5)));
			} else if (!row.IsNull(6)) {
				stats.column_stats.push_back(GetStatsValue("max", row.GetValue<string>(6)));
			}
			if (!row.IsNull(7)) {
				stats.column_stats.push_back(GetStatsValue("null_count", row.GetValue<string>(7)));
			}
			if (!row.IsNull(8)) {
				stats.column_stats.push_back(GetStatsValue("column_size_bytes", row.GetValue<string>(8)));
			}
			column.column_stats.push_back(std::move(stats));
		}
		// now
	}
	vector<DuckLakeDataFile> written_files;
	throw InternalException("FIXME");
}

DuckLakeAddDataFilesFunction::DuckLakeAddDataFilesFunction()
    : TableFunction("ducklake_add_data_files", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, DuckLakeAddDataFilesExecute, DuckLakeAddDataFilesBind, DuckLakeAddDataFilesInit) {
}

} // namespace duckdb
