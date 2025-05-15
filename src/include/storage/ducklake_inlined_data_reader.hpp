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
#include "common/ducklake_snapshot.hpp"

namespace duckdb {
class DuckLakeFieldData;
struct DuckLakeFunctionInfo;

class DuckLakeInlinedDataReader : public BaseFileReader {
public:
	//! Initialize an inlined data reader over a set of data stored within a table in the metadata catalog
	DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info, string table_name,
	                          vector<MultiFileColumnDefinition> columns);
	//! Initialize an inlined data reader over a set of data
	DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info,
	                          shared_ptr<DuckLakeInlinedData> data, vector<MultiFileColumnDefinition> columns);

public:
	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
	                       LocalTableFunctionState &lstate) override;
	void Scan(ClientContext &context, GlobalTableFunctionState &global_state, LocalTableFunctionState &local_state,
	          DataChunk &chunk) override;

	string GetReaderType() const override;

private:
	mutex lock;
	DuckLakeFunctionInfo &read_info;
	string table_name;
	shared_ptr<DuckLakeInlinedData> data;
	bool initialized_scan = false;
	ColumnDataScanState state;
};

} // namespace duckdb
