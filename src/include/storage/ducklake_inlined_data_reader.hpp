//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_inlined_data_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "storage/ducklake_inlined_data.hpp"

namespace duckdb {
class DuckLakeFieldData;

class DuckLakeInlinedDataReader : public BaseFileReader {
public:
	DuckLakeInlinedDataReader(const OpenFileInfo &info, const DuckLakeInlinedData &data, const DuckLakeFieldData &field_data);
public:
	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
								   LocalTableFunctionState &lstate) override;
	void Scan(ClientContext &context, GlobalTableFunctionState &global_state,
					  LocalTableFunctionState &local_state, DataChunk &chunk) override;

	string GetReaderType() const override;

private:
	mutex lock;
	const DuckLakeInlinedData &data;
	bool initialized_scan = false;
	ColumnDataScanState state;

};

} // namespace duckdb
