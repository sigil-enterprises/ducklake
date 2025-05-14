#include "storage/ducklake_inline_data.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

DuckLakeInlineData::DuckLakeInlineData(PhysicalOperator &child, idx_t inline_row_limit) :
	PhysicalOperator(PhysicalOperatorType::EXTENSION, child.types, child.estimated_cardinality), inline_row_limit(inline_row_limit) {
	children.push_back(child);
}

enum class InlinePhase {
	INLINING_ROWS,
	EMITTING_PREVIOUSLY_INLINED_ROWS,
	PASS_THROUGH_ROWS

};

class InlineDataState : public OperatorState {
public:
	explicit InlineDataState() {
	}

	InlinePhase phase = InlinePhase::INLINING_ROWS;
	unique_ptr<ColumnDataCollection> inlined_data;
};

class InlineDataGlobalState : public GlobalOperatorState {
public:
	explicit InlineDataGlobalState() : total_inlined_rows(0) {
	}

	atomic<idx_t> total_inlined_rows;
};

unique_ptr<OperatorState> DuckLakeInlineData::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<InlineDataState>();
}

unique_ptr<GlobalOperatorState> DuckLakeInlineData::GetGlobalOperatorState(ClientContext &context) const {
	return make_uniq<InlineDataGlobalState>();
}

OperatorResultType DuckLakeInlineData::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
						   GlobalOperatorState &gstate_p, OperatorState &state_p) const {
	auto &state = state_p.Cast<InlineDataState>();
	auto &gstate = gstate_p.Cast<InlineDataGlobalState>();
	if (state.phase == InlinePhase::PASS_THROUGH_ROWS) {
		// not inlining rows - forward the input directly
		chunk.Reference(input);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	if (state.phase == InlinePhase::EMITTING_PREVIOUSLY_INLINED_ROWS) {
		// we are emitting previously inlined rows - this happens if we decided not to inline data after all
		throw InternalException("FIXME: emitting previously inlined rows");

		// finished emitting previously inlined rows - destroy them and pass through any subsequent rows
		state.inlined_data.reset();
		state.phase = InlinePhase::PASS_THROUGH_ROWS;
	}
	// add the count to the global count and check if we are still inlining rows
	auto current_row_count = gstate.total_inlined_rows += input.size();
	if (current_row_count > inline_row_limit) {
		// we have exceeded the limit - start emitting rows from the `inlined_data`
		state.phase = InlinePhase::EMITTING_PREVIOUSLY_INLINED_ROWS;
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
	// we are inlining these rows - append to the collection
	if (!state.inlined_data) {
		state.inlined_data = make_uniq<ColumnDataCollection>(context.client, types);
	}
	state.inlined_data->Append(input);
	return OperatorResultType::NEED_MORE_INPUT;
}

OperatorFinalizeResultType DuckLakeInlineData::FinalExecute(ExecutionContext &context, DataChunk &chunk,
												GlobalOperatorState &gstate, OperatorState &state) const {
	// push inlined data to transaction
	throw InternalException("FIXME Inline FinalExecute");
}

string DuckLakeInlineData::GetName() const {
	return "DUCKLAKE_INLINE_DATA";
}

}
