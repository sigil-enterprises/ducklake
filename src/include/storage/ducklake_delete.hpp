//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_delete.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_insert.hpp"

namespace duckdb {
class DuckLakeTableEntry;
class DuckLakeDeleteGlobalState;

class DuckLakeDelete : public PhysicalOperator {
public:
	//! INSERT INTO
	DuckLakeDelete(DuckLakeTableEntry &table, PhysicalOperator &child,
	               unordered_map<string, DataFileIndex> delete_file_map);

	//! The table to delete from
	DuckLakeTableEntry &table;
	//! A map of filename -> data file index
	unordered_map<string, DataFileIndex> delete_file_map;

public:
	// // Source interface
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

private:
	void FlushDelete(ClientContext &context, DuckLakeDeleteGlobalState &global_state, const string &filename,
	                 vector<idx_t> &deleted_rows) const;
};

} // namespace duckdb
