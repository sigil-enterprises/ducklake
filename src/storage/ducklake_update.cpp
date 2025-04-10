#include "storage/ducklake_update.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_catalog.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

namespace duckdb {

DuckLakeUpdate::DuckLakeUpdate(DuckLakeTableEntry &table, PhysicalOperator &child, PhysicalOperator &copy_op, PhysicalOperator &delete_op, PhysicalOperator &insert_op)
	: PhysicalOperator(PhysicalOperatorType::EXTENSION, {LogicalType::BIGINT}, 1), table(table), copy_op(copy_op), delete_op(delete_op), insert_op(insert_op) {
	children.push_back(child);
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class DuckLakeUpdateGlobalState : public GlobalSinkState {
public:
	unique_ptr<GlobalSinkState> copy_global_state;
	unique_ptr<GlobalSinkState> delete_global_state;
	idx_t total_updated_count = 0;
};

class DuckLakeUpdateLocalState : public LocalSinkState {
public:
	unique_ptr<LocalSinkState> copy_local_state;
	unique_ptr<LocalSinkState> delete_local_state;
};

unique_ptr<GlobalSinkState> DuckLakeUpdate::GetGlobalSinkState(ClientContext &context) const {
	auto result = make_uniq<DuckLakeUpdateGlobalState>();
	result->copy_global_state = copy_op.GetGlobalSinkState(context);
	result->delete_global_state = delete_op.GetGlobalSinkState(context);
	return std::move(result);
}

unique_ptr<LocalSinkState> DuckLakeUpdate::GetLocalSinkState(ExecutionContext &context) const {
	auto result = make_uniq<DuckLakeUpdateLocalState>();
	result->copy_local_state = copy_op.GetLocalSinkState(context);
	result->delete_local_state = delete_op.GetLocalSinkState(context);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType DuckLakeUpdate::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeUpdateGlobalState>();
	auto &local_state = input.local_state.Cast<DuckLakeUpdateLocalState>();
	throw InternalException("FIXME: push data into copy/delete");
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
SinkCombineResultType DuckLakeUpdate::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeUpdateGlobalState>();
	auto &local_state = input.local_state.Cast<DuckLakeUpdateLocalState>();
	OperatorSinkCombineInput copy_combine_input {*global_state.copy_global_state, *local_state.copy_local_state, input.interrupt_state};
	auto result = copy_op.Combine(context, copy_combine_input);
	if (result != SinkCombineResultType::FINISHED) {
		throw InternalException("DuckLakeUpdate::Combine does not support async child operators");
	}
	OperatorSinkCombineInput del_combine_input {*global_state.delete_global_state, *local_state.delete_local_state, input.interrupt_state};
	result = delete_op.Combine(context, del_combine_input);
	if (result != SinkCombineResultType::FINISHED) {
		throw InternalException("DuckLakeUpdate::Combine does not support async child operators");
	}
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DuckLakeUpdate::Finalize(Pipeline &pipeline, Event &event, ClientContext &context, OperatorSinkFinalizeInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeUpdateGlobalState>();
	OperatorSinkFinalizeInput copy_finalize_input {*global_state.copy_global_state, input.interrupt_state};
	auto result = copy_op.Finalize(pipeline, event, context, copy_finalize_input);
	if (result != SinkFinalizeType::READY) {
		throw InternalException("DuckLakeUpdate::Finalize does not support async child operators");
	}
	OperatorSinkFinalizeInput del_finalize_input {*global_state.delete_global_state,input.interrupt_state};
	result = delete_op.Finalize(pipeline, event, context, del_finalize_input);
	if (result != SinkFinalizeType::READY) {
		throw InternalException("DuckLakeUpdate::Finalize does not support async child operators");
	}
	throw InternalException("FIXME: push data from copy op into insert_op");
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DuckLakeUpdate::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
	auto &global_state = sink_state->Cast<DuckLakeUpdateGlobalState>();
	auto value = Value::BIGINT(global_state.total_updated_count);
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, value);
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DuckLakeUpdate::GetName() const {
	return "DUCKLAKE_UPDATE";
}

InsertionOrderPreservingMap<string> DuckLakeUpdate::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name;
	return result;
}

PhysicalOperator &DuckLakeCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
PhysicalOperator &child_plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for updates of a DuckLake table");
	}
	for (auto &expr : op.expressions) {
		if (expr->type == ExpressionType::VALUE_DEFAULT) {
			throw BinderException("SET DEFAULT is not yet supported for updates of a DuckLake table");
		}
	}
	auto &table = op.table.Cast<DuckLakeTableEntry>();
	// updates are executed as a delete + insert - generate the two nodes (delete and insert)
	// plan the copy for the insert
	auto &copy_op = DuckLakeInsert::PlanCopyForInsert(context, planner, table, nullptr);
	// plan the delete
	auto &delete_op = DuckLakeDelete::PlanDelete(context, planner, table, child_plan);
	// plan the actual insert
	auto &insert_op = DuckLakeInsert::PlanInsert(context, planner, table);

	return planner.Make<DuckLakeUpdate>(table, child_plan, copy_op, delete_op, insert_op);
}

void DuckLakeTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
											   LogicalUpdate &update, ClientContext &context) {
	// all updates in DuckLake are deletes + inserts
	update.update_is_del_and_insert = true;

	// push projections for all columns
	physical_index_set_t all_columns;
	for (auto &column : GetColumns().Physical()) {
		all_columns.insert(column.Physical());
	}
	LogicalUpdate::BindExtraColumns(*this, get, proj, update, all_columns);
}

} // namespace duckdb
