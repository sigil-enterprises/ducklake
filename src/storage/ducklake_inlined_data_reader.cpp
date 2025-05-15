#include "storage/ducklake_inlined_data_reader.hpp"
#include "storage/ducklake_multi_file_reader.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_metadata_manager.hpp"

namespace duckdb {

DuckLakeInlinedDataReader::DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info,
                                                     string table_name_p, vector<MultiFileColumnDefinition> columns_p)
    : BaseFileReader(info), read_info(read_info), table_name(std::move(table_name_p)) {
	columns = std::move(columns_p);
}
DuckLakeInlinedDataReader::DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info,
                                                     shared_ptr<DuckLakeInlinedData> data_p,
                                                     vector<MultiFileColumnDefinition> columns_p)
    : BaseFileReader(info), read_info(read_info), data(std::move(data_p)) {
	columns = std::move(columns_p);
}

bool DuckLakeInlinedDataReader::TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
                                                  LocalTableFunctionState &lstate) {
	{
		// check if we are the reader responsible for scanning
		lock_guard<mutex> guard(lock);
		if (initialized_scan) {
			return false;
		}
		initialized_scan = true;
	}

	if (!data) {
		// scanning data from a table - read it from the metadata catalog
		auto &metadata_manager = read_info.transaction.GetMetadataManager();

		vector<string> columns_to_read;
		for (auto &column_id : column_indexes) {
			columns_to_read.push_back(columns[column_id.GetPrimaryIndex()].name);
		}
		data = metadata_manager.ReadInlinedData(read_info.snapshot, table_name, columns_to_read);
	}
	data->data->InitializeScan(state);
	return true;
}

void DuckLakeInlinedDataReader::Scan(ClientContext &context, GlobalTableFunctionState &global_state,
                                     LocalTableFunctionState &local_state, DataChunk &chunk) {
	data->data->Scan(state, chunk);
}

string DuckLakeInlinedDataReader::GetReaderType() const {
	return "DuckLake Inlined Data";
}

} // namespace duckdb
