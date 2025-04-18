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
#include "storage/ducklake_stats.hpp"

namespace duckdb {
class DuckLakeCatalog;
class DuckLakeTableEntry;
class DuckLakeFieldData;
struct DuckLakePartition;
struct DuckLakeCopyOptions;

class DuckLakeInsert : public PhysicalOperator {
public:
	//! INSERT INTO
	DuckLakeInsert(const vector<LogicalType> &types, DuckLakeTableEntry &table, optional_idx partition_id,
	               string encryption_key);
	//! CREATE TABLE AS
	DuckLakeInsert(const vector<LogicalType> &types, SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info,
	               string encryption_key);

	//! The table to insert into
	optional_ptr<DuckLakeTableEntry> table;
	//! Table schema, in case of CREATE TABLE AS
	optional_ptr<SchemaCatalogEntry> schema;
	//! Create table info, in case of CREATE TABLE AS
	unique_ptr<BoundCreateTableInfo> info;
	//! The partition id we are writing into (if any)
	optional_idx partition_id;
	//! The encryption key used for writing the Parquet files
	string encryption_key;

public:
	// // Source interface
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

	static DuckLakeColumnStats ParseColumnStats(const LogicalType &type, const vector<Value> stats);
	static DuckLakeCopyOptions GetCopyOptions(ClientContext &context, const ColumnList &columns,
													 optional_ptr<DuckLakePartition> partition_data,
													 optional_ptr<DuckLakeFieldData> field_data,
													 const string &data_path, string encryption_key, bool write_row_id);
	static PhysicalOperator &PlanCopyForInsert(ClientContext &context, PhysicalPlanGenerator &planner,
	                                           DuckLakeTableEntry &table, optional_ptr<PhysicalOperator> plan,
	                                           string encryption_key, bool write_row_id = false);
	static PhysicalOperator &PlanCopyForInsert(ClientContext &context, const ColumnList &columns,
	                                           PhysicalPlanGenerator &planner,
	                                           optional_ptr<DuckLakePartition> partition_data,
	                                           optional_ptr<DuckLakeFieldData> field_data,
	                                           optional_ptr<PhysicalOperator> plan, const string &data_path,
	                                           string encryption_key, bool write_row_id = false);
	static PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
	                                    DuckLakeTableEntry &table, string encryption_key);

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	// SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
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

struct DuckLakeCopyOptions {
	DuckLakeCopyOptions(unique_ptr<CopyInfo> info, CopyFunction copy_function);

	unique_ptr<CopyInfo> info;
	CopyFunction copy_function;
	unique_ptr<FunctionData> bind_data;

	string file_path;
	bool use_tmp_file;
	FilenamePattern filename_pattern;
	string file_extension;
	CopyOverwriteMode overwrite_mode;
	bool per_thread_output;
	optional_idx file_size_bytes;
	bool rotate;
	CopyFunctionReturnType return_type;

	bool partition_output;
	bool write_partition_columns;
	bool write_empty_file = true;
	vector<idx_t> partition_columns;
	vector<string> names;
	vector<LogicalType> expected_types;
};

} // namespace duckdb
