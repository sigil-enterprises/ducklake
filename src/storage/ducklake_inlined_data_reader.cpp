#include "storage/ducklake_inlined_data_reader.hpp"

namespace duckdb {

DuckLakeInlinedDataReader::DuckLakeInlinedDataReader(const OpenFileInfo &info) : BaseFileReader(info) {
	MultiFileColumnDefinition column("i", LogicalType::BIGINT);
	column.identifier = Value::INTEGER(1);
	columns.push_back(std::move(column));
}

bool DuckLakeInlinedDataReader::TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
							   LocalTableFunctionState &lstate) {
	lock_guard<mutex> guard(lock);
	if (initialized_scan) {
		return false;
	}
	initialized_scan = true;
	return true;
}

void DuckLakeInlinedDataReader::Scan(ClientContext &context, GlobalTableFunctionState &global_state,
				  LocalTableFunctionState &local_state, DataChunk &chunk) {
	if (row_idx > 0) {
		return;
	}
	chunk.SetValue(0, 0, Value::BIGINT(42));
	chunk.SetCardinality(1);
	row_idx++;
}

string DuckLakeInlinedDataReader::GetReaderType() const {
	return "DuckLake Inlined Data";
}

}
