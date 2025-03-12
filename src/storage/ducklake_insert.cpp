#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_insert.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"

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
	explicit DuckLakeInsertGlobalState(DuckLakeTableEntry &table) : table(table), total_insert_count(0) {
	}

	DuckLakeTableEntry &table;
	vector<DuckLakeDataFile> written_files;
	idx_t total_insert_count;
};

unique_ptr<GlobalSinkState> DuckLakeInsert::GetGlobalSinkState(ClientContext &context) const {
	optional_ptr<DuckLakeTableEntry> table_ptr;
	if (info) {
		// CREATE TABLE AS - create the table
		auto &catalog = schema->catalog;
		table_ptr = &catalog.CreateTable(catalog.GetCatalogTransaction(context), *schema.get_mutable(), *info)
		                 ->Cast<DuckLakeTableEntry>();
	} else {
		// INSERT INTO
		table_ptr = table;
	}
	return make_uniq<DuckLakeInsertGlobalState>(*table_ptr);
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

	for (idx_t r = 0; r < chunk.size(); r++) {
		DuckLakeDataFile data_file;
		data_file.file_name = chunk.GetValue(0, r).GetValue<string>();
		data_file.row_count = chunk.GetValue(1, r).GetValue<idx_t>();
		data_file.file_size_bytes = chunk.GetValue(2, r).GetValue<idx_t>();
		data_file.footer_size = chunk.GetValue(4, r).GetValue<idx_t>();

		// extract the column stats
		auto column_stats = chunk.GetValue(5, r);
		auto &map_children = MapValue::GetChildren(column_stats);
		for (idx_t col_idx = 0; col_idx < map_children.size(); col_idx++) {
			auto &struct_children = StructValue::GetChildren(map_children[col_idx]);
			auto &col_name = StringValue::Get(struct_children[0]);
			auto &col_stats = MapValue::GetChildren(struct_children[1]);
			auto column_names = DuckLakeUtil::ParseQuotedList(col_name, '.');
			if (column_names.size() != 1) {
				// FIXME: handle nested types
				continue;
			}
			auto &column_def = global_state.table.GetColumn(column_names[0]);

			DuckLakeColumnStats column_stats(column_def.Type());
			for (idx_t stats_idx = 0; stats_idx < col_stats.size(); stats_idx++) {
				auto &stats_children = StructValue::GetChildren(col_stats[stats_idx]);
				auto &stats_name = StringValue::Get(stats_children[0]);
				auto &stats_value = StringValue::Get(stats_children[1]);
				if (stats_name == "min") {
					D_ASSERT(!column_stats.has_min);
					column_stats.min = stats_value;
					column_stats.has_min = true;
				} else if (stats_name == "max") {
					D_ASSERT(!column_stats.has_max);
					column_stats.max = stats_value;
					column_stats.has_max = true;
				} else if (stats_name == "null_count") {
					D_ASSERT(!column_stats.has_null_count);
					column_stats.has_null_count = true;
					column_stats.null_count = std::stoull(stats_value);
				} else {
					throw NotImplementedException("Unsupported stats type in DuckLakeInsert::Sink()");
				}
			}

			data_file.column_stats.insert(make_pair(column_def.Oid(), std::move(column_stats)));
		}

		global_state.written_files.push_back(std::move(data_file));
		global_state.total_insert_count += data_file.row_count;
	}

	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DuckLakeInsert::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
	auto &global_state = sink_state->Cast<DuckLakeInsertGlobalState>();
	auto value = Value::BIGINT(global_state.total_insert_count);
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

	auto &transaction = DuckLakeTransaction::Get(context, global_state.table.catalog);
	transaction.AppendFiles(global_state.table.GetTableId(), global_state.written_files);

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
                                                                optional_ptr<DuckLakePartition> partition_data,
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

	auto copy_return_types = GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS);
	auto physical_copy =
	    make_uniq<PhysicalCopyToFile>(copy_return_types, copy_fun->function, std::move(function_data), 1);

	auto current_write_uuid = UUID::ToString(UUID::GenerateRandomUUID());

	physical_copy->use_tmp_file = false;
	if (partition_data) {
		vector<idx_t> partition_columns;
		for (auto &field : partition_data->fields) {
			partition_columns.push_back(field.column_id);
		}
		physical_copy->filename_pattern.SetFilenamePattern("ducklake-" + current_write_uuid + "_{i}");
		physical_copy->file_path = data_path;
		physical_copy->partition_output = true;
		physical_copy->partition_columns = std::move(partition_columns);
	} else {
		physical_copy->file_path = data_path + "/duckdblake-" + current_write_uuid + ".parquet";
		physical_copy->partition_output = false;
	}

	physical_copy->file_extension = "parquet";
	physical_copy->overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
	physical_copy->per_thread_output = false;
	physical_copy->rotate = false;
	physical_copy->return_type = CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
	physical_copy->write_partition_columns = true;
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
	auto &ducklake_table = op.table.Cast<DuckLakeTableEntry>();
	auto &columns = ducklake_table.GetColumns();
	auto partition_data = ducklake_table.GetPartitionData();

	auto physical_copy = PlanCopyForInsert(context, columns, partition_data, std::move(plan));
	auto insert = make_uniq<DuckLakeInsert>(op, ducklake_table, op.column_index_map);
	insert->children.push_back(std::move(physical_copy));
	return std::move(insert);
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanCreateTableAs(ClientContext &context, LogicalCreateTable &op,
                                                                unique_ptr<PhysicalOperator> plan) {
	auto &create_info = op.info->Base();
	auto &columns = create_info.columns;
	// FIXME: if table already exists and we are doing CREATE IF NOT EXISTS - skip
	auto physical_copy = PlanCopyForInsert(context, columns, nullptr, std::move(plan));
	auto insert = make_uniq<DuckLakeInsert>(op, op.schema, std::move(op.info));
	insert->children.push_back(std::move(physical_copy));
	return std::move(insert);
}

} // namespace duckdb
