#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_table_entry.hpp"
// #include "storage/ducklake_schema_entry.hpp"

namespace duckdb {
struct DuckLakeSetCommitMessageData : public TableFunctionData {
	DuckLakeSetCommitMessageData(Catalog &catalog, string author, string commit_message)
	    : catalog(catalog), author(std::move(author)), commit_message(std::move(commit_message)) {
	}
	Catalog &catalog;
	string author;
	string commit_message;
};

struct DuckLakeSetCommitMessageState : public GlobalTableFunctionState {
	DuckLakeSetCommitMessageState() {
	}

	bool finished = false;
};

unique_ptr<GlobalTableFunctionState> DuckLakeSetCommitMessageInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DuckLakeSetCommitMessageState>();
}

static unique_ptr<FunctionData> DuckLakeSetCommitMessageBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto author = StringValue::Get(input.inputs[1]);
	auto commit_message = StringValue::Get(input.inputs[2]);

	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	return make_uniq<DuckLakeSetCommitMessageData>(catalog,author, commit_message);
}

void DuckLakeSetCommitMessageExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeSetCommitMessageState>();
	auto &bind_data = data_p.bind_data->Cast<DuckLakeSetCommitMessageData>();
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	transaction.SetConfigOption(bind_data.option);
	state.finished = true;
}

DuckLakeSetCommitMessage::DuckLakeSetCommitMessage():TableFunction("ducklake_set_commit_message", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                    DuckLakeSetCommitMessageExecute, DuckLakeSetCommitMessageBind, DuckLakeSetCommitMessageInit) {
}
} //namespace duckdb