#include "functions/ducklake_table_functions.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

struct CleanupBindData : public TableFunctionData {
	explicit CleanupBindData(Catalog &catalog) : catalog(catalog) {
	}

	Catalog &catalog;
	vector<DuckLakeFileScheduledForCleanup> files;
	bool dry_run = false;
};

static unique_ptr<FunctionData> DuckLakeCleanupBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = BaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto result = make_uniq<CleanupBindData>(catalog);
	timestamp_tz_t from_timestamp;
	bool has_timestamp = false;
	bool cleanup_all = false;
	for (auto &entry : input.named_parameters) {
		if (StringUtil::CIEquals(entry.first, "dry_run")) {
			result->dry_run = true;
		} else if (StringUtil::CIEquals(entry.first, "cleanup_all")) {
			cleanup_all = true;
		} else if (StringUtil::CIEquals(entry.first, "older_than")) {
			from_timestamp = entry.second.GetValue<timestamp_tz_t>();
			has_timestamp = true;
		} else {
			throw InternalException("Unsupported named parameter for ducklake_cleanup_old_files");
		}
	}
	if (cleanup_all == has_timestamp) {
		throw InvalidInputException("ducklake_cleanup_old_files: either cleanup_all OR older_than must be specified");
	}
	// scan the files
	string filter;
	if (has_timestamp) {
		auto ts = Timestamp::ToString(timestamp_t(from_timestamp.value));
		filter = StringUtil::Format("WHERE schedule_start < TIMESTAMP '%s' AT TIME ZONE 'UTC'", ts);
	}
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	auto &metadata_manager = transaction.GetMetadataManager();
	result->files = metadata_manager.GetFilesScheduledForCleanup(filter);

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("path");

	return std::move(result);
}

struct DuckLakeCleanupData : public GlobalTableFunctionState {
	DuckLakeCleanupData() : offset(0), executed(false) {
	}

	idx_t offset;
	bool executed;
};

unique_ptr<GlobalTableFunctionState> DuckLakeCleanupInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckLakeCleanupData>();
	return std::move(result);
}

void DuckLakeCleanupExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<CleanupBindData>();
	auto &state = data_p.global_state->Cast<DuckLakeCleanupData>();
	if (state.offset >= data.files.size()) {
		return;
	}
	if (!state.executed && !data.dry_run) {
		// delete the files
		auto &fs = FileSystem::GetFileSystem(context);
		for (auto &file : data.files) {
			fs.RemoveFile(file.path);
		}
		// remove the files that are scheduled for cleanup
		auto &transaction = DuckLakeTransaction::Get(context, data.catalog);
		auto &metadata_manager = transaction.GetMetadataManager();
		metadata_manager.RemoveFilesScheduledForCleanup(data.files);
		state.executed = true;
	}
	idx_t count = 0;
	while (state.offset < data.files.size() && count < STANDARD_VECTOR_SIZE) {
		auto &file = data.files[state.offset++];
		output.SetValue(0, count, file.path);
		count++;
	}
	output.SetCardinality(count);
}

DuckLakeCleanupOldFilesFunction::DuckLakeCleanupOldFilesFunction()
    : TableFunction("ducklake_cleanup_old_files", {LogicalType::VARCHAR}, DuckLakeCleanupExecute, DuckLakeCleanupBind,
                    DuckLakeCleanupInit) {
	named_parameters["older_than"] = LogicalType::TIMESTAMP_TZ;
	named_parameters["cleanup_all"] = LogicalType::BOOLEAN;
	named_parameters["dry_run"] = LogicalType::BOOLEAN;
}

} // namespace duckdb
