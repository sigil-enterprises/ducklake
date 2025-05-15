//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_inlined_data_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/base_file_reader.hpp"

namespace duckdb {

class DuckLakeInlinedDataReader : public BaseFileReader {
public:
	DuckLakeInlinedDataReader(const OpenFileInfo &info);
public:
	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
								   LocalTableFunctionState &lstate) override;
	void Scan(ClientContext &context, GlobalTableFunctionState &global_state,
					  LocalTableFunctionState &local_state, DataChunk &chunk) override;

	string GetReaderType() const override;

private:
	mutex lock;
	bool initialized_scan = false;
	idx_t row_idx = 0;

};

} // namespace duckdb
