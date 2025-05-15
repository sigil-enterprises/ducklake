#include "storage/ducklake_inline_data.hpp"
#include "storage/ducklake_insert.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
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
	explicit InlineDataGlobalState(const DuckLakeInlineData &op) : op(op), total_inlined_rows(0) {
	}

	InlinePhase AddRows(InlineDataState &state, idx_t new_rows) {
		lock_guard<mutex> guard(lock);
		total_inlined_rows += new_rows;
		if (total_inlined_rows > op.inline_row_limit) {
			// we have exceeded the total amount of rows - bail
			if (global_inlined_data) {
				// if we have any global inlined data - add it to the local inlined data so we can emit it
				AddToCollection(std::move(global_inlined_data), state.inlined_data);
			}
			return InlinePhase::PASS_THROUGH_ROWS;
		}
		// we are still inlining rows
		return InlinePhase::INLINING_ROWS;
	}

    static void AddToCollection(unique_ptr<ColumnDataCollection> source, unique_ptr<ColumnDataCollection> &target) {
        if (!target) {
            target = std::move(source);
            return;
        }
        ColumnDataAppendState append_state;
        target->InitializeAppend(append_state);
        for(auto &chunk : source->Chunks()) {
            target->Append(append_state, chunk);
        }
    }

	const DuckLakeInlineData &op;
	mutex lock;
	idx_t total_inlined_rows = 0;
	InlinePhase global_phase = InlinePhase::INLINING_ROWS;
	unique_ptr<ColumnDataCollection> global_inlined_data;
};

unique_ptr<OperatorState> DuckLakeInlineData::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<InlineDataState>();
}

unique_ptr<GlobalOperatorState> DuckLakeInlineData::GetGlobalOperatorState(ClientContext &context) const {
	return make_uniq<InlineDataGlobalState>(*this);
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
	// we have a new batch of rows
	// add the count to the global count and check if we are still inlining rows
	auto new_phase = gstate.AddRows(state, input.size());
	if (new_phase != InlinePhase::INLINING_ROWS) {
		// we have exceeded the limit - start emitting rows from the `inlined_data`
		state.phase = InlinePhase::EMITTING_PREVIOUSLY_INLINED_ROWS;
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
	// we are inlining these rows - append to the local collection
	if (!state.inlined_data) {
		state.inlined_data = make_uniq<ColumnDataCollection>(context.client, types);
	}
	state.inlined_data->Append(input);
	return OperatorResultType::NEED_MORE_INPUT;
}

OperatorFinalizeResultType DuckLakeInlineData::FinalExecute(ExecutionContext &context, DataChunk &chunk,
												GlobalOperatorState &gstate_p, OperatorState &state_p) const {
	auto &state = state_p.Cast<InlineDataState>();
	auto &gstate = gstate_p.Cast<InlineDataGlobalState>();
	if (!state.inlined_data) {
		// no inlined data, we are done
		return OperatorFinalizeResultType::FINISHED;
	}
	// push the inlined data into the global inlined data
	lock_guard<mutex> guard(gstate.lock);
	if (gstate.total_inlined_rows > inline_row_limit) {
		throw InternalException("FIXME: emit inlined rows again");
	}
	InlineDataGlobalState::AddToCollection(std::move(state.inlined_data), gstate.global_inlined_data);
	return OperatorFinalizeResultType::FINISHED;
}

OperatorFinalResultType DuckLakeInlineData::OperatorFinalize(Pipeline &pipeline, Event &event, ClientContext &context, OperatorFinalizeInput &input) const {
	// push inlined data to transaction
	auto &gstate = input.global_state.Cast<InlineDataGlobalState>();
	if (!gstate.global_inlined_data) {
		// no inlined data, we are done
		return OperatorFinalResultType::FINISHED;
	}
	auto &insert_gstate = insert->sink_state->Cast<DuckLakeInsertGlobalState>();
	if (insert_gstate.total_insert_count != 0) {
		throw InternalException("Inlining rows but also inserting rows through a file");
	}
	// set the insert count to the total number of inlined rows
	insert_gstate.total_insert_count = gstate.global_inlined_data->Count();
	auto &table = insert_gstate.table;
	auto &transaction = DuckLakeTransaction::Get(context, table.ParentCatalog());
	// push the inlined data into the transaction
	transaction.AppendInlinedData(table.GetTableId(), std::move(gstate.global_inlined_data));
	return OperatorFinalResultType::FINISHED;
}

string DuckLakeInlineData::GetName() const {
	return "DUCKLAKE_INLINE_DATA";
}

}
