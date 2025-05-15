#include "storage/ducklake_inlined_data_reader.hpp"
#include "storage/ducklake_multi_file_reader.hpp"

namespace duckdb {

DuckLakeInlinedDataReader::DuckLakeInlinedDataReader(const OpenFileInfo &info, const DuckLakeInlinedData &data, const DuckLakeFieldData &field_data) : BaseFileReader(info), data(data) {
	columns = DuckLakeMultiFileReader::ColumnsFromFieldData(field_data);
}

bool DuckLakeInlinedDataReader::TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
							   LocalTableFunctionState &lstate) {
	lock_guard<mutex> guard(lock);
	if (initialized_scan) {
		return false;
	}
	initialized_scan = true;
	data.data->InitializeScan(state);
	return true;
}

void DuckLakeInlinedDataReader::Scan(ClientContext &context, GlobalTableFunctionState &global_state,
				  LocalTableFunctionState &local_state, DataChunk &chunk) {
	data.data->Scan(state, chunk);
}

string DuckLakeInlinedDataReader::GetReaderType() const {
	return "DuckLake Inlined Data";
}

}
