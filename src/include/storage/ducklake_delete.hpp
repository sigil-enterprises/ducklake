//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_delete.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_insert.hpp"
#include "storage/ducklake_delete_filter.hpp"
#include "storage/ducklake_metadata_info.hpp"

namespace duckdb {
class DuckLakeTableEntry;
class DuckLakeDeleteGlobalState;
class DuckLakeTransaction;

struct DuckLakeDeleteMap {
	unordered_map<string, DuckLakeFileListExtendedEntry> file_map;
	unordered_map<string, shared_ptr<DuckLakeDeleteData>> delete_data_map;
};

class DuckLakeDelete : public PhysicalOperator {
public:
	DuckLakeDelete(DuckLakeTableEntry &table, PhysicalOperator &child, shared_ptr<DuckLakeDeleteMap> delete_map,
	               string encryption_key);

	//! The table to delete from
	DuckLakeTableEntry &table;
	//! A map of filename -> data file index and filename -> delete data
	shared_ptr<DuckLakeDeleteMap> delete_map;
	//! The encryption key used to encrypt the written files
	string encryption_key;

public:
	// // Source interface
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

	static PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
	                                    DuckLakeTableEntry &table, PhysicalOperator &child_plan, string encryption_key);

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
	void FlushDelete(DuckLakeTransaction &transaction, ClientContext &context, DuckLakeDeleteGlobalState &global_state,
	                 const string &filename, vector<idx_t> &deleted_rows) const;
};

} // namespace duckdb
