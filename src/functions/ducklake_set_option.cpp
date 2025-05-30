#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"

namespace duckdb {

struct DuckLakeSetOptionData : public TableFunctionData {
	DuckLakeSetOptionData(Catalog &catalog, string option_p, string value_p)
	    : catalog(catalog), option(std::move(option_p)), value(std::move(value_p)) {
	}

	Catalog &catalog;
	string option;
	string value;
};

static unique_ptr<FunctionData> DuckLakeSetOptionBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto option = StringUtil::Lower(StringValue::Get(input.inputs[1]));
	auto &val = input.inputs[2];

	string value;
	if (option == "parquet_compression") {
		auto codec = val.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>();
		vector<string> supported_algorithms {"uncompressed", "snappy", "gzip", "zstd", "brotli", "lz4"};
		bool found = false;
		for (auto &algorithm : supported_algorithms) {
			if (StringUtil::CIEquals(algorithm, codec)) {
				found = true;
				break;
			}
		}
		if (!found) {
			auto supported = StringUtil::Join(supported_algorithms, ", ");
			throw NotImplementedException("Unsupported codec \"%s\" for parquet, supported options are %s", codec,
			                              supported);
		}
		value = StringUtil::Lower(codec);
	} else if (option == "parquet_version") {
		auto version = val.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();
		if (version != 1 && version != 2) {
			throw NotImplementedException("Only Parquet version 1 and 2 are supported");
		}
		value = "V" + to_string(version);
	} else if (option == "parquet_compression_level") {
		auto compression_level = val.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();
		value = to_string(compression_level);
	} else if (option == "parquet_row_group_size") {
		auto row_group_size = val.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();
		if (row_group_size == 0) {
			throw NotImplementedException("Row group size cannot be 0");
		}
		value = to_string(row_group_size);
	} else {
		throw NotImplementedException("Unsupported option %s", option);
	}
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	return make_uniq<DuckLakeSetOptionData>(catalog, option, value);
}

struct DuckLakeSetOptionState : public GlobalTableFunctionState {
	DuckLakeSetOptionState() {
	}

	bool finished = false;
};

unique_ptr<GlobalTableFunctionState> DuckLakeSetOptionInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DuckLakeSetOptionState>();
}

void DuckLakeSetOptionExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeSetOptionState>();
	auto &bind_data = data_p.bind_data->Cast<DuckLakeSetOptionData>();
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	transaction.SetConfigOption(bind_data.option, bind_data.value);
	state.finished = true;
}

DuckLakeSetOptionFunction::DuckLakeSetOptionFunction()
    : TableFunction("ducklake_set_option", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
                    DuckLakeSetOptionExecute, DuckLakeSetOptionBind, DuckLakeSetOptionInit) {
}

} // namespace duckdb
