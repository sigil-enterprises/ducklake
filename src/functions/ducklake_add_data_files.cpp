#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_insert.hpp"

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
	optional_idx row_count;
	optional_idx file_size;
};

struct DuckLakeFileProcessor {
public:
	DuckLakeFileProcessor(DuckLakeTransaction &transaction, DuckLakeTableEntry &table) :
		transaction(transaction), table(table) {}

	vector<DuckLakeDataFile> AddFiles(const vector<string> &globs);

private:
	void ReadParquetSchema(const string &glob);
	void ReadParquetStats(const string &glob);
	void ReadParquetFileMetadata(const string &glob);
	DuckLakeDataFile AddFileToTable(ParquetFileMetadata &file);
	DuckLakeNameMapEntry MapColumn(ParquetColumn &column, const DuckLakeFieldId &field_id, DuckLakeDataFile &file);

	Value GetStatsValue(string name, Value val);
private:
	DuckLakeTransaction &transaction;
	DuckLakeTableEntry &table;
	unordered_map<string, unique_ptr<ParquetFileMetadata>> parquet_files;
};

void DuckLakeFileProcessor::ReadParquetSchema(const string &glob) {
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
}

Value DuckLakeFileProcessor::GetStatsValue(string name, Value val) {
	child_list_t<Value> children;
	children.emplace_back("key", Value(name));
	children.emplace_back("value", std::move(val));
	return Value::STRUCT(std::move(children));
}

void DuckLakeFileProcessor::ReadParquetStats(const string &glob) {
	auto result = transaction.Query(StringUtil::Format(R"(
SELECT file_name, column_id, num_values, coalesce(stats_min, stats_min_value), coalesce(stats_max, stats_max_value), stats_null_count, total_compressed_size
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
			// stats.column_stats.push_back(GetStatsValue("value_count", row.GetValue<string>(2)));
		}
		if (!row.IsNull(3)) {
			stats.column_stats.push_back(GetStatsValue("min", row.GetValue<string>(3)));
		}
		if (!row.IsNull(4)) {
			stats.column_stats.push_back(GetStatsValue("max", row.GetValue<string>(4)));
		}
		if (!row.IsNull(5)) {
			stats.column_stats.push_back(GetStatsValue("null_count", row.GetValue<string>(5)));
		}
		if (!row.IsNull(6)) {
			stats.column_stats.push_back(GetStatsValue("column_size_bytes", row.GetValue<string>(6)));
		}
		column.column_stats.push_back(std::move(stats));
	}
}

void DuckLakeFileProcessor::ReadParquetFileMetadata(const string &glob) {
	// use read_blob to get the file size
	// FIXME: we should obtain the footer size as well at this point
	auto result = transaction.Query(StringUtil::Format(R"(
SELECT filename, size
FROM read_blob(%s)
)", SQLString(glob)));
	for(auto &row : *result) {
		auto filename = row.GetValue<string>(0);
		auto entry = parquet_files.find(filename);
		if (entry == parquet_files.end()) {
			throw InvalidInputException("Parquet file was returned by parquet_metadata, but not returned by parquet_schema - did a Parquet file get added to a glob while processing?");
		}
		entry->second->file_size = row.GetValue<idx_t>(1);
	}
	// use parquet_file_metadata to get the num rows
	result = transaction.Query(StringUtil::Format(R"(
SELECT file_name, num_rows
FROM parquet_file_metadata(%s)
)", SQLString(glob)));
	for(auto &row : *result) {
		auto filename = row.GetValue<string>(0);
		auto entry = parquet_files.find(filename);
		if (entry == parquet_files.end()) {
			throw InvalidInputException("Parquet file was returned by parquet_metadata, but not returned by parquet_schema - did a Parquet file get added to a glob while processing?");
		}
		entry->second->row_count = row.GetValue<idx_t>(1);
	}
}

DuckLakeNameMapEntry DuckLakeFileProcessor::MapColumn(ParquetColumn &column, const DuckLakeFieldId &field_id, DuckLakeDataFile &file) {
	// FIXME: check if types of the columns are compatible
	if (column.field_id.IsValid()) {
		throw InvalidInputException("File has field ids defined - only mapping by name is supported currently");
	}

	DuckLakeNameMapEntry map_entry;
	map_entry.source_name = column.name;
	map_entry.target_field_id = field_id.GetFieldIndex();
	if (field_id.HasChildren()) {
		throw InternalException("FIXME: remap child field ids");
	}
	// parse the per row-group stats
	vector<DuckLakeColumnStats> row_group_stats_list;
	for(auto &stats : column.column_stats) {
		auto row_group_stats = DuckLakeInsert::ParseColumnStats(field_id.Type(), stats.column_stats);
		row_group_stats_list.push_back(std::move(row_group_stats));
	}
	// merge all stats into the first one
	for(idx_t i = 1; i < row_group_stats_list.size(); i++) {
		row_group_stats_list[0].MergeStats(row_group_stats_list[i]);
	}
	// add the final stats of this column to the file
	file.column_stats.emplace(field_id.GetFieldIndex(), std::move(row_group_stats_list[0]));
	return map_entry;
}

DuckLakeDataFile DuckLakeFileProcessor::AddFileToTable(ParquetFileMetadata &file) {
	DuckLakeDataFile result;
	result.file_name = file.filename;
	result.row_count = file.row_count.GetIndex();
	result.file_size_bytes = file.file_size.GetIndex();

	// map columns from the file to the table
	auto &field_data = table.GetFieldData();
	auto &field_ids = field_data.GetFieldIds();

	// create a top-level map of columns
	case_insensitive_map_t<const_reference<DuckLakeFieldId>> field_id_map;
	for(auto &field_id : field_ids) {
		field_id_map.emplace(field_id->Name(), *field_id);
	}

	DuckLakeNameMap name_map;
	name_map.table_id = table.GetTableId();
	for(auto &col : file.columns) {
		// find the top-level column to map to
		auto entry = field_id_map.find(col->name);
		if (entry == field_id_map.end()) {
			throw InvalidInputException("Column \"%s\" exists in file %s but was not found in the table", col->name, file.filename);
		}
		name_map.column_maps.push_back(MapColumn(*col, entry->second.get(), result));
		field_id_map.erase(entry);
	}
	for(auto &entry : field_id_map) {
		throw InvalidInputException("Column \"%s\" exists in table %s but was not found in file %s", entry.second.get().Name(), file.filename);
	}
	// we successfully mapped this file - register the name map and refer to it in the file
	result.mapping_id = transaction.AddNameMap(std::move(name_map));
	return result;
}

vector<DuckLakeDataFile> DuckLakeFileProcessor::AddFiles(const vector<string> &globs) {
	// fetch the metadata, stats and columns from the various files
	for(auto &glob : globs) {
		// query the parquet_schema to figure out the schema for each of the columns
		ReadParquetSchema(glob);
		// query the parquet_metadata to get the stats for each of the columns
		ReadParquetStats(glob);
		// read parquet file metadata
		ReadParquetFileMetadata(glob);
	}

	// now we have obtained a list of files to add together with the relevant information (statistics, file size, ...)
	// we need to create a mapping from the columns in the file to the columns in the table
	vector<DuckLakeDataFile> written_files;
	for(auto &entry : parquet_files) {
		written_files.push_back(AddFileToTable(*entry.second));
	}
	return written_files;
}

void DuckLakeAddDataFilesExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeAddDataFilesState>();
	auto &bind_data = data_p.bind_data->Cast<DuckLakeAddDataFilesData>();
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);

	if (state.finished) {
		return;
	}
	DuckLakeFileProcessor processor(transaction, bind_data.table);
	auto files_to_add = processor.AddFiles(bind_data.globs);
	// add the files
	transaction.AppendFiles(bind_data.table.GetTableId(), std::move(files_to_add));
	state.finished = true;
}

DuckLakeAddDataFilesFunction::DuckLakeAddDataFilesFunction()
    : TableFunction("ducklake_add_data_files", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, DuckLakeAddDataFilesExecute, DuckLakeAddDataFilesBind, DuckLakeAddDataFilesInit) {
}

} // namespace duckdb
