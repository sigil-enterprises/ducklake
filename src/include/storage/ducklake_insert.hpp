//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_insert.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/index_vector.hpp"

namespace duckdb {
class DuckLakeTableEntry;

class DuckLakeInsert : public PhysicalOperator {
public:
	//! INSERT INTO
	DuckLakeInsert(LogicalOperator &op, DuckLakeTableEntry &table, optional_idx partition_id);
	//! CREATE TABLE AS
	DuckLakeInsert(LogicalOperator &op, SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info);

	//! The table to insert into
	optional_ptr<DuckLakeTableEntry> table;
	//! Table schema, in case of CREATE TABLE AS
	optional_ptr<SchemaCatalogEntry> schema;
	//! Create table info, in case of CREATE TABLE AS
	unique_ptr<BoundCreateTableInfo> info;
	//! The physical copy used internally by this insert
	unique_ptr<PhysicalOperator> physical_copy_to_file;
	//! The partition id we are writing into (if any)
	optional_idx partition_id;

public:
	// // Source interface
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	// SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	// unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
