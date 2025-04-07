#include "storage/ducklake_catalog.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "storage/ducklake_scan.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "common/ducklake_data_file.hpp"

namespace duckdb {

DuckLakeDelete::DuckLakeDelete(DuckLakeTableEntry &table, PhysicalOperator &child)
    : PhysicalOperator(PhysicalOperatorType::EXTENSION, {LogicalType::BIGINT}, 1), table(table) {
    children.push_back(child);
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
struct WrittenColumnInfo {
	WrittenColumnInfo() = default;
	WrittenColumnInfo(LogicalType type_p, int32_t field_id) : type(std::move(type_p)), field_id(field_id) {}

	LogicalType type;
	int32_t field_id;
};

class DuckLakeDeleteGlobalState : public GlobalSinkState {
public:
	explicit DuckLakeDeleteGlobalState() {
		written_columns["file_path"] = WrittenColumnInfo(LogicalType::VARCHAR, 2147483546);
		written_columns["pos"] = WrittenColumnInfo(LogicalType::BIGINT, 2147483545);
	}

	vector<DuckLakeDataFile> written_files;
	unordered_map<string, WrittenColumnInfo> written_columns;
	idx_t total_deleted_count = 0;
};

unique_ptr<GlobalSinkState> DuckLakeDelete::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DuckLakeDeleteGlobalState>();
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType DuckLakeDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeDeleteGlobalState>();

	for (idx_t r = 0; r < chunk.size(); r++) {
		DuckLakeDataFile data_file;
		data_file.file_name = chunk.GetValue(0, r).GetValue<string>();
		data_file.row_count = chunk.GetValue(1, r).GetValue<idx_t>();
		data_file.file_size_bytes = chunk.GetValue(2, r).GetValue<idx_t>();
		data_file.footer_size = chunk.GetValue(3, r).GetValue<idx_t>();

		// extract the column stats
		auto column_stats = chunk.GetValue(4, r);
		auto &map_children = MapValue::GetChildren(column_stats);
		for (idx_t col_idx = 0; col_idx < map_children.size(); col_idx++) {
			auto &struct_children = StructValue::GetChildren(map_children[col_idx]);
			auto &col_name = StringValue::Get(struct_children[0]);
			auto &col_stats = MapValue::GetChildren(struct_children[1]);
			auto &entry = global_state.written_columns[col_name];

			auto column_stats = DuckLakeInsert::ParseColumnStats(entry.type, col_stats);
			data_file.column_stats.insert(make_pair(entry.field_id, std::move(column_stats)));
		}
		global_state.written_files.push_back(std::move(data_file));
		global_state.total_deleted_count += data_file.row_count;
	}

	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DuckLakeDelete::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
	auto &global_state = sink_state->Cast<DuckLakeDeleteGlobalState>();
	auto value = Value::BIGINT(global_state.total_deleted_count);
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, value);
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DuckLakeDelete::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
 	auto &global_state = input.global_state.Cast<DuckLakeDeleteGlobalState>();

	throw InternalException("FIXME: flush deleted files");

	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DuckLakeDelete::GetName() const {
	return "DUCKLAKE_DELETE";
}

InsertionOrderPreservingMap<string> DuckLakeDelete::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name;
	return result;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
unique_ptr<LogicalOperator> ExtractLogicalGet(reference<unique_ptr<LogicalOperator>> op) {
	while (op.get()->type != LogicalOperatorType::LOGICAL_GET) {
		if (op.get()->type == LogicalOperatorType::LOGICAL_FILTER) {
			op = op.get()->children[0];
		} else {
			throw NotImplementedException("Unimplemented logical operator type in DuckLake Delete");
		}
	}
	auto res = std::move(op.get());
	op.get() = make_uniq<LogicalDummyScan>(0);
	return res;
}

idx_t GenerateScanForDelete(ClientContext &context, LogicalGet &get, unique_ptr<LogicalOperator> &op, const string &filename) {
	// we need to push the FILE_ROW_NUMBER column through the stack
	// MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER;
	switch(op->type) {
	case LogicalOperatorType::LOGICAL_FILTER: {
		// no need to push anything - just recurse
		auto &filter = op->Cast<LogicalFilter>();
		if (!filter.projection_map.empty()) {
			throw InternalException("FIXME: projection map");
		}
		return GenerateScanForDelete(context, get, op->children[0], filename);
	}
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN: {
		// generate the scan for this node
		auto function = DuckLakeFunctions::GetDuckLakeScanFunction(*context.db);
		function.function_info = get.function.function_info;

		auto bind_data = DuckLakeFunctions::BindDuckLakeScan(context, function);

		// we explicitly only scan one file in this scan
		vector<string> paths;
		paths.push_back(filename);
		auto &multi_file_bind_data = bind_data->Cast<MultiFileBindData>();
		multi_file_bind_data.file_list = make_shared_ptr<SimpleMultiFileList>(std::move(paths));

		// generate the get
		auto virtual_columns = get.virtual_columns;
		virtual_columns.insert(make_pair(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER,
								TableColumn("file_row_number", LogicalType::BIGINT)));
		multi_file_bind_data.virtual_columns = virtual_columns;
		auto scan = make_uniq<LogicalGet>(0, get.function, std::move(bind_data), get.returned_types, get.names, std::move(virtual_columns));
		auto column_ids = get.GetColumnIds();
		idx_t file_row_number_idx = column_ids.size() - 1;
		scan->SetColumnIds(std::move(column_ids));
		op = std::move(scan);
		return file_row_number_idx;
	}
	default:
		throw NotImplementedException("Unimplemented logical operator type in DuckLake GenerateFileDelete");
	}
}
unique_ptr<LogicalOperator> DuckLakeCatalog::GenerateFileDelete(ClientContext &context, LogicalGet &get, LogicalOperator &op, const string &filename) {
	// generate a scan for the delete for this specific file
	auto new_op = op.Copy(context);
	idx_t file_row_idx = GenerateScanForDelete(context, get, new_op, filename);

	// generate a projection that contains (1) the filename, and (2) the file row number
	vector<unique_ptr<Expression>> select_list;
	select_list.push_back(make_uniq<BoundConstantExpression>(Value(filename)));
	select_list.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, file_row_idx));
	auto proj = make_uniq<LogicalProjection>(0, std::move(select_list));
	proj->children.push_back(std::move(new_op));

	// FIXME: if the file already has deletes - perform a union + order by

	// generate the copy that writes the delete file
	auto &fs = FileSystem::GetFileSystem(context);
	auto delete_file_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	string delete_file_path = fs.JoinPath(DataPath(), delete_file_uuid);

	auto info = make_uniq<CopyInfo>();
	info->file_path = delete_file_path;
	info->format = "parquet";
	info->is_from = false;

	// generate the field ids to be written by the parquet writer
	// these field ids follow icebergs' ids and names for the delete files
	child_list_t<Value> values;
	values.emplace_back("file_path", Value::INTEGER(2147483546));
	values.emplace_back("pos", Value::INTEGER(2147483545));
	auto field_ids = Value::STRUCT(std::move(values));
	vector<Value> field_input;
	field_input.push_back(std::move(field_ids));
	info->options["field_ids"] = std::move(field_input);

	// get the actual copy function and bind it
	auto &copy_fun = GetCopyFunction(*context.db, "parquet");

	CopyFunctionBindInput bind_input(*info);

	vector<string> names_to_write { "file_path", "pos" };
	vector<LogicalType> types_to_write { LogicalType::VARCHAR, LogicalType::BIGINT };

	auto function_data = copy_fun.function.copy_to_bind(context, bind_input, names_to_write, types_to_write);

	// generate the logical copy to file
	auto copy = make_uniq<LogicalCopyToFile>(copy_fun.function, std::move(function_data), std::move(info));

	copy->use_tmp_file = false;
	copy->file_path = delete_file_path;
	copy->partition_output = false;
	copy->write_empty_file = false;
	copy->file_extension = "parquet";
	copy->overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
	copy->per_thread_output = false;
	copy->rotate = false;
	copy->return_type = CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
	copy->write_partition_columns = false;

	copy->children.push_back(std::move(proj));
	copy->ResolveOperatorTypes();

	return std::move(copy);
}

PhysicalOperator &DuckLakeCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalDelete &op) {
	auto logical_get = ExtractLogicalGet(op.children[0]);
	auto &get = logical_get->Cast<LogicalGet>();
	auto &bind_data = get.bind_data->Cast<MultiFileBindData>();
	auto files = bind_data.file_list->GetAllFiles();

	// push the file row number as a projected column
	auto &mutable_ids = get.GetMutableColumnIds();
	for(idx_t i = 0; i < mutable_ids.size(); i++) {
		if (mutable_ids[i].IsRowIdColumn()) {
			mutable_ids[i] = ColumnIndex(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER);
		}
	}

	// generate the individual file deletes
	vector<unique_ptr<LogicalOperator>> file_deletes;
	for(auto &file : files) {
		auto new_op = GenerateFileDelete(context, get, *op.children[0], file);
		file_deletes.push_back(std::move(new_op));
	}
	unique_ptr<LogicalOperator> result;
	if (!file_deletes.empty()) {
		while (file_deletes.size() > 1) {
			vector<unique_ptr<LogicalOperator>> new_nodes;
			for (idx_t i = 0; i < file_deletes.size(); i += 2) {
				if (i + 1 == file_deletes.size()) {
					new_nodes.push_back(std::move(file_deletes[i]));
				} else {
					auto copy_union = make_uniq<LogicalSetOperation>(0, 1U, std::move(file_deletes[i]),
																	 std::move(file_deletes[i + 1]),
																	 LogicalOperatorType::LOGICAL_UNION, true, false);
					new_nodes.push_back(std::move(copy_union));
				}
			}
			file_deletes = std::move(new_nodes);
		}
		result = std::move(file_deletes[0]);
	} else {
		throw InternalException("FIXME: all deletes have been pruned");
	}

	// create the plan and push a DuckLakeDelete node
	auto &child_plan = planner.CreatePlan(*result);
	return planner.Make<DuckLakeDelete>(op.table.Cast<DuckLakeTableEntry>(), child_plan);
}

PhysicalOperator &DuckLakeCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	throw InternalException("Unsupported DuckLake function");
}

} // namespace duckdb
