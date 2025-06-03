#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include <unordered_map>

namespace duckdb {

struct OptionMetadata {
	string description;
	string type;
};

static constexpr const char *OPTION_TYPE_USER = "user";
static constexpr const char *OPTION_TYPE_SYSTEM = "system";
static constexpr const char *DEFAULT_OPTION_DESCRIPTION = "Custom configuration option";

static constexpr const char *COLUMN_NAME = "name";
static constexpr const char *COLUMN_VALUE = "value";
static constexpr const char *COLUMN_TYPE = "type";
static constexpr const char *COLUMN_DESCRIPTION = "description";

static const std::unordered_map<string, OptionMetadata> DUCKLAKE_OPTIONS = {
    // User-configurable options
    {"parquet_compression",
     {"Compression algorithm for Parquet files (uncompressed, snappy, gzip, zstd, brotli, lz4)", OPTION_TYPE_USER}},
    {"parquet_version", {"Parquet format version (1 or 2)", OPTION_TYPE_USER}},
    {"parquet_compression_level", {"Compression level for Parquet files", OPTION_TYPE_USER}},
    {"parquet_row_group_size", {"Number of rows per row group in Parquet files", OPTION_TYPE_USER}},

    // System options
    {"version", {"DuckLake format version", OPTION_TYPE_SYSTEM}},
    {"created_by", {"DuckLake creator information", OPTION_TYPE_SYSTEM}},
    {"data_path", {"Path to data files", OPTION_TYPE_SYSTEM}},
    {"encrypted", {"Whether the DuckLake is encrypted", OPTION_TYPE_SYSTEM}}};

struct DuckLakeGetOptionData : public TableFunctionData {
	DuckLakeGetOptionData(Catalog &catalog, string option_filter_p, SchemaIndex schema_id_p = SchemaIndex(),
	                      TableIndex table_id_p = TableIndex())
	    : catalog(catalog), option_filter(std::move(option_filter_p)), schema_id(schema_id_p), table_id(table_id_p) {
	}

	Catalog &catalog;
	string option_filter;
	SchemaIndex schema_id;
	TableIndex table_id;
};

struct DuckLakeOptionInfo {
	string name;
	string value;
	string type;
	string description;
};

struct DuckLakeGetOptionState : public GlobalTableFunctionState {
	DuckLakeGetOptionState() : offset(0) {
	}

	vector<DuckLakeOptionInfo> options;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckLakeGetOptionBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);

	string option_filter;
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		option_filter = StringUtil::Lower(StringValue::Get(input.inputs[1]));
	}

	SchemaIndex schema_id;
	TableIndex table_id;
	string schema;
	string table;

	auto schema_entry = input.named_parameters.find("schema");
	if (schema_entry != input.named_parameters.end() && !schema_entry->second.IsNull()) {
		schema = StringValue::Get(schema_entry->second);
	}
	auto table_entry = input.named_parameters.find("table_name");
	if (table_entry != input.named_parameters.end() && !table_entry->second.IsNull()) {
		table = StringValue::Get(table_entry->second);
	}

	if (!table.empty()) {

		auto table_catalog_entry =
		    catalog.GetEntry<TableCatalogEntry>(context, schema, table, OnEntryNotFound::THROW_EXCEPTION);
		auto &ducklake_table = table_catalog_entry->Cast<DuckLakeTableEntry>();
		table_id = ducklake_table.GetTableId();
		if (table_id.IsTransactionLocal()) {
			throw NotImplementedException("Settings cannot be retrieved for transaction-local tables");
		}

		if (!schema.empty()) {
			auto schema_catalog_entry = catalog.GetSchema(context, schema, OnEntryNotFound::THROW_EXCEPTION);
			auto &ducklake_schema = schema_catalog_entry->Cast<DuckLakeSchemaEntry>();
			schema_id = ducklake_schema.GetSchemaId();
			if (schema_id.IsTransactionLocal()) {
				throw NotImplementedException("Settings cannot be retrieved for transaction-local schemas");
			}
		}
	} else if (!schema.empty()) {
		// find the schema scope
		auto schema_catalog_entry = catalog.GetSchema(context, schema, OnEntryNotFound::THROW_EXCEPTION);
		auto &ducklake_schema = schema_catalog_entry->Cast<DuckLakeSchemaEntry>();
		schema_id = ducklake_schema.GetSchemaId();
		if (schema_id.IsTransactionLocal()) {
			throw NotImplementedException("Settings cannot be retrieved for transaction-local schemas");
		}
	}

	names.emplace_back(COLUMN_NAME);
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back(COLUMN_VALUE);
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back(COLUMN_TYPE);
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back(COLUMN_DESCRIPTION);
	return_types.emplace_back(LogicalType::VARCHAR);

	return make_uniq<DuckLakeGetOptionData>(catalog, option_filter, schema_id, table_id);
}

static string GetOptionDescription(const string &option_name) {
	auto it = DUCKLAKE_OPTIONS.find(option_name);
	if (it != DUCKLAKE_OPTIONS.end()) {
		return it->second.description;
	}
	return DEFAULT_OPTION_DESCRIPTION;
}

static string GetOptionType(const string &option_name) {
	auto it = DUCKLAKE_OPTIONS.find(option_name);
	if (it != DUCKLAKE_OPTIONS.end()) {
		return it->second.type;
	}
	return OPTION_TYPE_USER;
}

unique_ptr<GlobalTableFunctionState> DuckLakeGetOptionInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<DuckLakeGetOptionData>();
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	auto &metadata_manager = transaction.GetMetadataManager();

	auto result = make_uniq<DuckLakeGetOptionState>();
	auto metadata = metadata_manager.LoadDuckLake();

	if (!bind_data.option_filter.empty()) {
		string option_value;
		bool found = false;

		if (bind_data.table_id.IsValid()) {
			for (auto &table_setting : metadata.table_settings) {
				if (table_setting.table_id == bind_data.table_id &&
				    StringUtil::Lower(table_setting.tag.key) == bind_data.option_filter) {
					option_value = table_setting.tag.value;
					found = true;
					break;
				}
			}
		}

		if (!found && bind_data.schema_id.IsValid()) {
			for (auto &schema_setting : metadata.schema_settings) {
				if (schema_setting.schema_id == bind_data.schema_id &&
				    StringUtil::Lower(schema_setting.tag.key) == bind_data.option_filter) {
					option_value = schema_setting.tag.value;
					found = true;
					break;
				}
			}
		}

		if (!found) {
			for (auto &tag : metadata.tags) {
				if (StringUtil::Lower(tag.key) == bind_data.option_filter) {
					option_value = tag.value;
					found = true;
					break;
				}
			}
		}

		if (found) {
			DuckLakeOptionInfo option_info;
			option_info.name = bind_data.option_filter;
			option_info.value = option_value;
			option_info.type = GetOptionType(bind_data.option_filter);
			option_info.description = GetOptionDescription(bind_data.option_filter);
			result->options.push_back(std::move(option_info));
		}
	} else {
		unordered_map<string, string> resolved_options;

		for (auto &tag : metadata.tags) {
			resolved_options[StringUtil::Lower(tag.key)] = tag.value;
		}

		if (bind_data.schema_id.IsValid()) {
			for (auto &schema_setting : metadata.schema_settings) {
				if (schema_setting.schema_id == bind_data.schema_id) {
					resolved_options[StringUtil::Lower(schema_setting.tag.key)] = schema_setting.tag.value;
				}
			}
		}

		if (bind_data.table_id.IsValid()) {
			for (auto &table_setting : metadata.table_settings) {
				if (table_setting.table_id == bind_data.table_id) {
					resolved_options[StringUtil::Lower(table_setting.tag.key)] = table_setting.tag.value;
				}
			}
		}

		for (auto &entry : resolved_options) {
			DuckLakeOptionInfo option_info;
			option_info.name = entry.first;
			option_info.value = entry.second;
			option_info.type = GetOptionType(entry.first);
			option_info.description = GetOptionDescription(entry.first);
			result->options.push_back(std::move(option_info));
		}
	}

	std::sort(result->options.begin(), result->options.end(),
	          [](const DuckLakeOptionInfo &a, const DuckLakeOptionInfo &b) { return a.name < b.name; });
	return std::move(result);
}

void DuckLakeGetOptionExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeGetOptionState>();

	if (state.offset >= state.options.size()) {
		return;
	}

	idx_t count = 0;
	while (state.offset < state.options.size() && count < STANDARD_VECTOR_SIZE) {
		auto &option = state.options[state.offset++];
		output.SetValue(0, count, Value(option.name));
		output.SetValue(1, count, Value(option.value));
		output.SetValue(2, count, Value(option.type));
		output.SetValue(3, count, Value(option.description));
		count++;
	}
	output.SetCardinality(count);
}

DuckLakeGetOptionFunction::DuckLakeGetOptionFunction()
    : BaseMetadataFunction("ducklake_get_option", DuckLakeGetOptionBind) {
	init_global = DuckLakeGetOptionInit;
	function = DuckLakeGetOptionExecute;
	named_parameters["table_name"] = LogicalType::VARCHAR;
	named_parameters["schema"] = LogicalType::VARCHAR;
}

TableFunctionSet DuckLakeGetOptionFunction::GetFunctions() {
	TableFunctionSet functions("ducklake_get_option");

	// Function with just catalog parameter (get all options)
	auto get_all_options =
	    TableFunction({LogicalType::VARCHAR}, DuckLakeGetOptionExecute, DuckLakeGetOptionBind, DuckLakeGetOptionInit);
	get_all_options.named_parameters["table_name"] = LogicalType::VARCHAR;
	get_all_options.named_parameters["schema"] = LogicalType::VARCHAR;
	functions.AddFunction(get_all_options);

	// Function with catalog and option_name parameters (get specific option)
	auto get_specific_option = TableFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, DuckLakeGetOptionExecute,
	                                         DuckLakeGetOptionBind, DuckLakeGetOptionInit);
	get_specific_option.named_parameters["table_name"] = LogicalType::VARCHAR;
	get_specific_option.named_parameters["schema"] = LogicalType::VARCHAR;
	functions.AddFunction(get_specific_option);

	return functions;
}

} // namespace duckdb
