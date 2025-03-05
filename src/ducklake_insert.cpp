#include "ducklake_catalog.hpp"
#include "ducklake_insert.hpp"
#include "ducklake_table_entry.hpp"
#include "ducklake_transaction.hpp"

#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

DuckLakeInsert::DuckLakeInsert(LogicalOperator &op, DuckLakeTableEntry &table,
                               physical_index_vector_t<idx_t> column_index_map_p)
    : PhysicalOperator(PhysicalOperatorType::EXTENSION, op.types, 1), table(&table), schema(nullptr),
      column_index_map(std::move(column_index_map_p)) {
}

DuckLakeInsert::DuckLakeInsert(LogicalOperator &op, SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info)
    : PhysicalOperator(PhysicalOperatorType::EXTENSION, op.types, 1), table(nullptr), schema(&schema),
      info(std::move(info)) {
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class DuckLakeInsertGlobalState : public GlobalSinkState {
public:
	explicit DuckLakeInsertGlobalState() : insert_count(0) {
	}
	vector<string> written_files;
	idx_t insert_count; // TODO: this needs to be per file
};

unique_ptr<GlobalSinkState> DuckLakeInsert::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DuckLakeInsertGlobalState>();
}

//
// unique_ptr<LocalSinkState> DuckLakeInsert::GetLocalSinkState(ExecutionContext &context) const {
//     return physical_copy_to_file->GetLocalSinkState(context);
// }

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType DuckLakeInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();

	if (chunk.size() != 1) {
		throw InternalException("DuckLakeInsert::Sink expects a single row containing output of the PhysicalCopy that "
		                        "should be its Source");
	}

	global_state.insert_count += chunk.GetValue(0, 0).GetValue<idx_t>();

	auto files = chunk.GetValue(1, 0);
	for (const auto &val : ListValue::GetChildren(files)) {
		global_state.written_files.push_back(val.ToString());
	}

	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DuckLakeInsert::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
	auto &global_state = sink_state->Cast<DuckLakeInsertGlobalState>();
	auto value = Value::BIGINT(global_state.insert_count);
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, value);
	return SourceResultType::FINISHED;
}
//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DuckLakeInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();

	optional_ptr<DuckLakeTableEntry> table_ptr;
	if (info) {
		auto &catalog = schema->catalog;
		table_ptr = &catalog.CreateTable(catalog.GetCatalogTransaction(context), *schema.get_mutable(), *info)
		                 ->Cast<DuckLakeTableEntry>();
	} else {
		table_ptr = table;
	}
	auto &transaction = DuckLakeTransaction::Get(context, table_ptr->catalog);
	transaction.AppendFiles(table_ptr->GetTableId(), global_state.written_files);

	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DuckLakeInsert::GetName() const {
	return table ? "DUCKLAKE_INSERT" : "DUCKLAKE_CREATE_TABLE_AS";
}

InsertionOrderPreservingMap<string> DuckLakeInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table ? table->name : info->Base().table;
	return result;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
static optional_ptr<CopyFunctionCatalogEntry> TryGetCopyFunction(DatabaseInstance &db, const string &name) {
	D_ASSERT(!name.empty());
	auto &system_catalog = Catalog::GetSystemCatalog(db);
	auto data = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = system_catalog.GetSchema(data, DEFAULT_SCHEMA);
	return schema.GetEntry(data, CatalogType::COPY_FUNCTION_ENTRY, name)->Cast<CopyFunctionCatalogEntry>();
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanCopyForInsert(ClientContext &context, const ColumnList &columns,
                                                                unique_ptr<PhysicalOperator> plan) {
	auto info = make_uniq<CopyInfo>();
	info->file_path = DataPath();
	info->format = "parquet";
	info->is_from = false;

	// Get Parquet Copy function
	auto copy_fun = TryGetCopyFunction(*context.db, "parquet");
	if (!copy_fun) {
		throw MissingExtensionException("Did not find parquet copy function required to write to DuckLake table");
	}

	//! FIXME: we only need to do this if this is a local path
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.DirectoryExists(data_path)) {
		fs.CreateDirectory(data_path);
	}

	// Bind Copy Function
	CopyFunctionBindInput bind_input(*info);

	auto names_to_write = columns.GetColumnNames();
	auto types_to_write = columns.GetColumnTypes();

	auto function_data = copy_fun->function.copy_to_bind(context, bind_input, names_to_write, types_to_write);

	auto copy_return_types = GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST);
	auto physical_copy =
	    make_uniq<PhysicalCopyToFile>(copy_return_types, copy_fun->function, std::move(function_data), 1);

	auto current_write_uuid = UUID::ToString(UUID::GenerateRandomUUID());

	physical_copy->use_tmp_file = false;
	physical_copy->file_path = data_path + "/duckdblake-" + current_write_uuid + ".parquet";
	physical_copy->partition_output = false;

	physical_copy->file_extension = "parquet";
	physical_copy->overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
	physical_copy->per_thread_output = false;
	physical_copy->rotate = false;
	physical_copy->return_type = CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST;
	physical_copy->children.push_back(std::move(plan));
	physical_copy->names = names_to_write;
	physical_copy->expected_types = types_to_write;

	return std::move(physical_copy);
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanInsert(ClientContext &context, LogicalInsert &op,
                                                         unique_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for insertion into DuckLake table");
	}
	if (op.action_type != OnConflictAction::THROW) {
		throw BinderException("ON CONFLICT clause not yet supported for insertion into DuckLake table");
	}
	auto &columns = op.table.GetColumns();
	auto physical_copy = PlanCopyForInsert(context, columns, std::move(plan));
	auto insert = make_uniq<DuckLakeInsert>(op, op.table.Cast<DuckLakeTableEntry>(), op.column_index_map);
	insert->children.push_back(std::move(physical_copy));
	return std::move(insert);
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanCreateTableAs(ClientContext &context, LogicalCreateTable &op,
                                                                unique_ptr<PhysicalOperator> plan) {
	auto &create_info = op.info->Base();
	auto &columns = create_info.columns;
	// FIXME: if table already exists and we are doing CREATE IF NOT EXISTS - skip
	auto physical_copy = PlanCopyForInsert(context, columns, std::move(plan));
	auto insert = make_uniq<DuckLakeInsert>(op, op.schema, std::move(op.info));
	insert->children.push_back(std::move(physical_copy));
	return std::move(insert);
}

} // namespace duckdb
