#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_field_data.hpp"
#include "storage/ducklake_insert.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"
#include "storage/ducklake_scan.hpp"

#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

DuckLakeInsert::DuckLakeInsert(const vector<LogicalType> &types, DuckLakeTableEntry &table, optional_idx partition_id, string encryption_key_p)
    : PhysicalOperator(PhysicalOperatorType::EXTENSION, types, 1), table(&table), schema(nullptr),
      partition_id(partition_id), encryption_key(std::move(encryption_key_p)) {
}

DuckLakeInsert::DuckLakeInsert(const vector<LogicalType> &types, SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info, string encryption_key_p)
    : PhysicalOperator(PhysicalOperatorType::EXTENSION, types, 1), table(nullptr), schema(&schema),
      info(std::move(info)), encryption_key(std::move(encryption_key_p))  {
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class DuckLakeInsertGlobalState : public GlobalSinkState {
public:
	explicit DuckLakeInsertGlobalState(DuckLakeTableEntry &table)
	    : table(table), total_insert_count(0), not_null_fields(table.GetNotNullFields()) {
	}

	DuckLakeTableEntry &table;
	vector<DuckLakeDataFile> written_files;
	idx_t total_insert_count;
	case_insensitive_set_t not_null_fields;
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

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
DuckLakeColumnStats DuckLakeInsert::ParseColumnStats(const LogicalType &type, const vector<Value> col_stats) {
	DuckLakeColumnStats column_stats(type);
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
			column_stats.null_count = StringUtil::ToUnsigned(stats_value);
		} else if (stats_name == "column_size_bytes") {
			column_stats.column_size_bytes = StringUtil::ToUnsigned(stats_value);
		} else if (stats_name == "has_nan") {
			column_stats.has_contains_nan = true;
			column_stats.contains_nan = stats_value == "true";
		} else {
			throw NotImplementedException("Unsupported stats type \"%s\" in DuckLakeInsert::Sink()",
										  stats_name);
		}
	}
	return column_stats;
}

SinkResultType DuckLakeInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();

	for (idx_t r = 0; r < chunk.size(); r++) {
		DuckLakeDataFile data_file;
		data_file.file_name = chunk.GetValue(0, r).GetValue<string>();
		data_file.row_count = chunk.GetValue(1, r).GetValue<idx_t>();
		data_file.file_size_bytes = chunk.GetValue(2, r).GetValue<idx_t>();
		data_file.footer_size = chunk.GetValue(3, r).GetValue<idx_t>();
		data_file.encryption_key = encryption_key;
		if (partition_id.IsValid()) {
			data_file.partition_id = partition_id.GetIndex();
		}

		// extract the column stats
		auto column_stats = chunk.GetValue(4, r);
		auto &map_children = MapValue::GetChildren(column_stats);
		auto &table = global_state.table;
		for (idx_t col_idx = 0; col_idx < map_children.size(); col_idx++) {
			auto &struct_children = StructValue::GetChildren(map_children[col_idx]);
			auto &col_name = StringValue::Get(struct_children[0]);
			auto &col_stats = MapValue::GetChildren(struct_children[1]);
			auto column_names = DuckLakeUtil::ParseQuotedList(col_name, '.');

			auto &field_id = table.GetFieldId(column_names);
			auto column_stats = ParseColumnStats(field_id.Type(), col_stats);
			if (column_stats.null_count > 0 && column_names.size() == 1) {
				// we wrote NULL values to a base column - verify NOT NULL constraint
				if (global_state.not_null_fields.count(column_names[0])) {
					throw ConstraintException("NOT NULL constraint failed: %s.%s", table.name, column_names[0]);
				}
			}

			data_file.column_stats.insert(make_pair(field_id.GetFieldIndex(), std::move(column_stats)));
		}
		// extract the partition info
		auto partition_info = chunk.GetValue(5, r);
		if (!partition_info.IsNull()) {
			auto &partition_children = MapValue::GetChildren(partition_info);
			for (idx_t col_idx = 0; col_idx < partition_children.size(); col_idx++) {
				auto &struct_children = StructValue::GetChildren(partition_children[col_idx]);
				auto &part_value = StringValue::Get(struct_children[1]);

				DuckLakeFilePartition file_partition_info;
				file_partition_info.partition_column_idx = col_idx;
				file_partition_info.partition_value = part_value;
				data_file.partition_values.push_back(std::move(file_partition_info));
			}
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
	transaction.AppendFiles(global_state.table.GetTableId(), std::move(global_state.written_files));

	return SinkFinalizeType::READY;
}

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
CopyFunctionCatalogEntry &DuckLakeFunctions::GetCopyFunction(DatabaseInstance &db, const string &name) {
	D_ASSERT(!name.empty());
	auto &system_catalog = Catalog::GetSystemCatalog(db);
	auto data = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = system_catalog.GetSchema(data, DEFAULT_SCHEMA);
	return schema.GetEntry(data, CatalogType::COPY_FUNCTION_ENTRY, name)->Cast<CopyFunctionCatalogEntry>();
}

Value GetFieldIdValue(const DuckLakeFieldId &field_id) {
	auto field_id_value = Value::BIGINT(field_id.GetFieldIndex().index);
	if (!field_id.HasChildren()) {
		// primitive type - return the field-id directly
		return field_id_value;
	}
	// nested type - generate a struct and recurse into children
	child_list_t<Value> values;
	values.emplace_back("__duckdb_field_id", std::move(field_id_value));
	for (auto &child : field_id.Children()) {
		values.emplace_back(child->Name(), GetFieldIdValue(*child));
	}
	return Value::STRUCT(std::move(values));
}

Value WrittenFieldIds(DuckLakeFieldData &field_data) {
	child_list_t<Value> values;
	for (idx_t c_idx = 0; c_idx < field_data.GetColumnCount(); c_idx++) {
		auto &field_id = field_data.GetByRootIndex(PhysicalIndex(c_idx));
		values.emplace_back(field_id.Name(), GetFieldIdValue(field_id));
	}
	return Value::STRUCT(std::move(values));
}

PhysicalOperator &DuckLakeInsert::PlanCopyForInsert(ClientContext &context, const ColumnList &columns,
                                                     PhysicalPlanGenerator &planner,
                                                     optional_ptr<DuckLakePartition> partition_data,
                                                     optional_ptr<DuckLakeFieldData> field_data,
                                                     optional_ptr<PhysicalOperator> plan, const string &data_path, string encryption_key) {
	auto info = make_uniq<CopyInfo>();
	info->file_path = data_path;
	info->format = "parquet";
	info->is_from = false;
	// generate the field ids to be written by the parquet writer
	shared_ptr<DuckLakeFieldData> generated_ids;
	if (!field_data) {
		// CTAS - generate new ids from columns
		generated_ids = DuckLakeFieldData::FromColumns(columns);
	}
	auto &field_ids = field_data ? *field_data : *generated_ids;
	vector<Value> field_input;
	field_input.push_back(WrittenFieldIds(field_ids));
	info->options["field_ids"] = std::move(field_input);
	if (!encryption_key.empty()) {
		child_list_t<Value> values;
		values.emplace_back("footer_key_value", Value::BLOB_RAW(encryption_key));
		vector<Value> encryption_input;
		encryption_input.push_back(Value::STRUCT(std::move(values)));
		info->options["encryption_config"] = std::move(encryption_input);
	}

	// Get Parquet Copy function
	auto &copy_fun = DuckLakeFunctions::GetCopyFunction(*context.db, "parquet");

	//! FIXME: we only need to do this if this is a local path
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.DirectoryExists(data_path)) {
		fs.CreateDirectory(data_path);
	}

	// Bind Copy Function
	CopyFunctionBindInput bind_input(*info);

	auto names_to_write = columns.GetColumnNames();
	auto types_to_write = columns.GetColumnTypes();

	auto function_data = copy_fun.function.copy_to_bind(context, bind_input, names_to_write, types_to_write);

	auto copy_return_types = GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS);
	auto &physical_copy =
	    planner.Make<PhysicalCopyToFile>(copy_return_types, copy_fun.function, std::move(function_data), 1)
	        .Cast<PhysicalCopyToFile>();

	physical_copy.use_tmp_file = false;
	if (partition_data) {
		vector<idx_t> partition_columns;
		for (auto &field : partition_data->fields) {
			partition_columns.push_back(field.column_id);
		}
		physical_copy.filename_pattern.SetFilenamePattern("ducklake-{uuid}");
		physical_copy.file_path = data_path;
		physical_copy.partition_output = true;
		physical_copy.partition_columns = std::move(partition_columns);
		physical_copy.write_empty_file = true;
	} else {
		auto current_write_uuid = UUID::ToString(UUID::GenerateRandomUUID());
		physical_copy.file_path = data_path + "/duckdblake-" + current_write_uuid + ".parquet";
		physical_copy.partition_output = false;
		physical_copy.write_empty_file = false;
	}

	physical_copy.file_extension = "parquet";
	physical_copy.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
	physical_copy.per_thread_output = false;
	physical_copy.rotate = false;
	physical_copy.return_type = CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
	physical_copy.write_partition_columns = true;
	if (plan) {
		physical_copy.children.push_back(*plan);
	}
	physical_copy.names = names_to_write;
	physical_copy.expected_types = types_to_write;

	return physical_copy;
}

PhysicalOperator &DuckLakeInsert::PlanCopyForInsert(ClientContext &context, PhysicalPlanGenerator &planner, DuckLakeTableEntry &table, optional_ptr<PhysicalOperator> plan, string encryption_key) {
	auto &columns = table.GetColumns();
	auto partition_data = table.GetPartitionData();
	optional_idx partition_id;
	if (partition_data) {
		partition_id = partition_data->partition_id;
	}
	auto &field_data = table.GetFieldData();
	return PlanCopyForInsert(context, columns, planner, partition_data, field_data, plan, table.DataPath(), std::move(encryption_key));
}

PhysicalOperator &DuckLakeInsert::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, DuckLakeTableEntry &table, string encryption_key) {
	auto partition_data = table.GetPartitionData();
	optional_idx partition_id;
	if (partition_data) {
		partition_id = partition_data->partition_id;
	}
	vector<LogicalType> return_types;
	return_types.emplace_back(LogicalType::BIGINT);
	return planner.Make<DuckLakeInsert>(return_types, table, partition_id, std::move(encryption_key));
}

string DuckLakeCatalog::GenerateEncryptionKey(ClientContext &context) const {
	if (Encryption() != DuckLakeEncryption::ENCRYPTED) {
		// not encrypted
		return string();
	}
	// generate an encryption key
	auto &engine = RandomEngine::Get(context);
	static constexpr const idx_t ENCRYPTION_KEY_SIZE = 16;
	data_t bytes[ENCRYPTION_KEY_SIZE];
	for (int i = 0; i < ENCRYPTION_KEY_SIZE; i += 4) {
		*reinterpret_cast<uint32_t *>(bytes + i) = engine.NextRandomInteger();
	}
	return string(char_ptr_cast(bytes), ENCRYPTION_KEY_SIZE);
}

PhysicalOperator &DuckLakeCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for insertion into DuckLake table");
	}
	if (op.action_type != OnConflictAction::THROW) {
		throw BinderException("ON CONFLICT clause not yet supported for insertion into DuckLake table");
	}
	if (!op.column_index_map.empty()) {
		plan = planner.ResolveDefaultsProjection(op, *plan);
	}
	string encryption_key = GenerateEncryptionKey(context);
	auto &ducklake_table = op.table.Cast<DuckLakeTableEntry>();
	auto &physical_copy = DuckLakeInsert::PlanCopyForInsert(context, planner, ducklake_table, plan, encryption_key);
	auto &insert = DuckLakeInsert::PlanInsert(context, planner, ducklake_table, std::move(encryption_key));
	insert.children.push_back(physical_copy);
	return insert;
}

PhysicalOperator &DuckLakeCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op, PhysicalOperator &plan) {
	auto &create_info = op.info->Base();
	auto &columns = create_info.columns;
	// FIXME: if table already exists and we are doing CREATE IF NOT EXISTS - skip
	string encryption_key = GenerateEncryptionKey(context);
	auto &physical_copy = DuckLakeInsert::PlanCopyForInsert(context, columns, planner, nullptr, nullptr, plan, DataPath(), encryption_key);
	auto &insert = planner.Make<DuckLakeInsert>(op.types, op.schema, std::move(op.info), std::move(encryption_key));
	insert.children.push_back(physical_copy);
	return insert;
}

} // namespace duckdb
