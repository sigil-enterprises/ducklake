#include "common/ducklake_types.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_transaction.hpp"

#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"

namespace duckdb {

DuckLakeTableEntry::DuckLakeTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       TableIndex table_id, string table_uuid_p,
                                       shared_ptr<DuckLakeFieldData> field_data_p, FieldIndex next_column_id_p,
                                       LocalChange local_change)
    : TableCatalogEntry(catalog, schema, info), table_id(table_id), table_uuid(std::move(table_uuid_p)),
      field_data(std::move(field_data_p)), next_column_id(next_column_id_p), local_change(local_change) {
	for (auto &constraint : info.constraints) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL:
			break;
		case ConstraintType::CHECK:
			throw NotImplementedException("CHECK constraints are not supported in DuckLake");
		case ConstraintType::UNIQUE:
			throw NotImplementedException("PRIMARY KEY/UNIQUE constraints are not supported in DuckLake");
		case ConstraintType::FOREIGN_KEY:
			throw NotImplementedException("FOREIGN KEY constraints are not supported in DuckLake");
		default:
			throw NotImplementedException("Unsupported constraint in DuckLake");
		}
	}
}

// ALTER TABLE RENAME/SET COMMENT/ADD COLUMN/DROP COLUMN
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, LocalChange local_change)
    : DuckLakeTableEntry(parent.ParentCatalog(), parent.ParentSchema(), info, parent.GetTableId(),
                         parent.GetTableUUID(), parent.field_data, parent.next_column_id, local_change) {
	if (parent.partition_data) {
		partition_data = make_uniq<DuckLakePartition>(*parent.partition_data);
	}
	if (local_change.type == LocalChangeType::ADD_COLUMN) {
		LogicalIndex new_col_idx(columns.LogicalColumnCount() - 1);
		auto &new_col = GetColumn(new_col_idx);
		field_data = DuckLakeFieldData::AddColumn(*field_data, new_col, next_column_id.index);
	}
}

// ALTER TABLE RENAME COLUMN
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, LocalChange local_change,
                                       const string &new_name)
    : DuckLakeTableEntry(parent, info, local_change) {
	D_ASSERT(local_change.type == LocalChangeType::RENAME_COLUMN);
	field_data = DuckLakeFieldData::RenameColumn(*field_data, local_change.field_index, new_name);
}

// ALTER TABLE DROP COLUMN
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, LocalChange local_change,
                                       unique_ptr<ColumnChangeInfo> changed_fields_p)
    : DuckLakeTableEntry(parent, info, local_change) {
	D_ASSERT(local_change.type == LocalChangeType::REMOVE_COLUMN);
	changed_fields = std::move(changed_fields_p);
}

// ALTER TABLE SET DATA TYPE
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info, LocalChange local_change,
                                       unique_ptr<ColumnChangeInfo> changed_fields_p,
                                       shared_ptr<DuckLakeFieldData> new_field_data)
    : DuckLakeTableEntry(parent, info, local_change) {
	D_ASSERT(local_change.type == LocalChangeType::CHANGE_COLUMN_TYPE);
	changed_fields = std::move(changed_fields_p);
	field_data = std::move(new_field_data);
}

// ALTER TABLE SET PARTITION KEY
DuckLakeTableEntry::DuckLakeTableEntry(DuckLakeTableEntry &parent, CreateTableInfo &info,
                                       unique_ptr<DuckLakePartition> partition_data_p)
    : DuckLakeTableEntry(parent, info, LocalChangeType::SET_PARTITION_KEY) {
	partition_data = std::move(partition_data_p);
}

const DuckLakeFieldId &DuckLakeTableEntry::GetFieldId(PhysicalIndex column_index) const {
	return field_data->GetByRootIndex(column_index);
}

optional_ptr<const DuckLakeFieldId> DuckLakeTableEntry::GetFieldId(FieldIndex field_index) const {
	return field_data->GetByFieldIndex(field_index);
}

const DuckLakeFieldId &DuckLakeTableEntry::GetFieldId(const vector<string> &column_names) const {
	auto &root_col = columns.GetColumn(column_names[0]);
	return field_data->GetByNames(root_col.Physical(), column_names);
}

const ColumnDefinition &DuckLakeTableEntry::GetColumnByFieldId(FieldIndex field_index) const {
	auto field_id = GetFieldId(field_index);
	if (!field_id) {
		throw InternalException("Column with field id %d not found", field_index.index);
	}
	return GetColumn(field_id->Name());
}

unique_ptr<BaseStatistics> GetColumnStats(const DuckLakeFieldId &field_id, const DuckLakeTableStats &table_stats) {
	auto &field_children = field_id.Children();
	if (field_children.empty()) {
		// non-nested type - lookup the field id in the stats map
		auto entry = table_stats.column_stats.find(field_id.GetFieldIndex());
		if (entry == table_stats.column_stats.end()) {
			return nullptr;
		}
		return entry->second.ToStats();
	}
	// nested type
	switch (field_id.Type().id()) {
	case LogicalTypeId::STRUCT: {
		auto struct_stats = StructStats::CreateUnknown(field_id.Type());
		for (idx_t child_idx = 0; child_idx < field_children.size(); ++child_idx) {
			auto child_stats = GetColumnStats(*field_children[child_idx], table_stats);
			StructStats::SetChildStats(struct_stats, child_idx, std::move(child_stats));
		}
		return struct_stats.ToUnique();
	}
	case LogicalTypeId::LIST: {
		auto list_stats = ListStats::CreateUnknown(field_id.Type());
		auto child_stats = GetColumnStats(*field_children[0], table_stats);
		ListStats::SetChildStats(list_stats, std::move(child_stats));
		return list_stats.ToUnique();
	}
	default:
		// unsupported nested type
		return nullptr;
	}
}

case_insensitive_set_t DuckLakeTableEntry::GetNotNullFields() const {
	case_insensitive_set_t result;
	for (auto &constraint : GetConstraints()) {
		if (constraint->type != ConstraintType::NOT_NULL) {
			throw InternalException("Unsupported constraint type in DuckLakeInsert");
		}
		auto &not_null = constraint->Cast<NotNullConstraint>();
		auto &col = GetColumn(not_null.index);
		result.insert(col.Name());
	}
	return result;
}

unique_ptr<BaseStatistics> DuckLakeTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	auto table_stats = GetTableStats(context);
	if (!table_stats) {
		return nullptr;
	}
	auto &field_id = field_data->GetByRootIndex(PhysicalIndex(column_id));
	return GetColumnStats(field_id, *table_stats);
}

TableFunction DuckLakeTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	throw InternalException("DuckLakeTableEntry::GetScanFunction called without entry lookup info");
}

TableFunction DuckLakeTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
                                                  const EntryLookupInfo &lookup_info) {
	auto function = DuckLakeFunctions::GetDuckLakeScanFunction(*context.db);
	auto &transaction = DuckLakeTransaction::Get(context, ParentCatalog());

	auto function_info =
	    make_shared_ptr<DuckLakeFunctionInfo>(*this, transaction.GetSnapshot(lookup_info.GetAtClause()));
	function_info->table_name = name;
	for (auto &col : columns.Logical()) {
		function_info->column_names.push_back(col.Name());
		function_info->column_types.push_back(col.Type());
	}
	function_info->table_id = GetTableId();
	function.function_info = std::move(function_info);

	vector<Value> inputs {Value("")};
	named_parameter_map_t param_map;
	vector<LogicalType> return_types;
	vector<string> names;
	TableFunctionRef empty_ref;

	TableFunctionBindInput bind_input(inputs, param_map, return_types, names, nullptr, nullptr, function, empty_ref);

	auto result = function.bind(context, bind_input, return_types, names);
	bind_data = std::move(result);

	return function;
}

void DuckLakeTableEntry::SetPartitionData(unique_ptr<DuckLakePartition> partition_data_p) {
	partition_data = std::move(partition_data_p);
}

optional_ptr<DuckLakeTableStats> DuckLakeTableEntry::GetTableStats(ClientContext &context) {
	auto &transaction = DuckLakeTransaction::Get(context, ParentCatalog());
	return GetTableStats(transaction);
}

optional_ptr<DuckLakeTableStats> DuckLakeTableEntry::GetTableStats(DuckLakeTransaction &transaction) {
	if (IsTransactionLocal()) {
		// no stats for transaction local tables
		return nullptr;
	}
	auto &dl_catalog = catalog.Cast<DuckLakeCatalog>();
	if (transaction.HasTransactionLocalChanges(GetTableId())) {
		// no stats if there are transaction-local changes
		return nullptr;
	}
	return dl_catalog.GetTableStats(transaction, GetTableId());
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, RenameTableInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	table_info.table = info.new_table_name;
	// create a complete copy of this table with only the name changed
	return make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChangeType::RENAMED);
}

DuckLakePartitionField GetPartitionField(DuckLakeTableEntry &table, ParsedExpression &expr) {
	if (expr.type != ExpressionType::COLUMN_REF) {
		throw NotImplementedException("Unsupported partition key %s - only columns are supported", expr.ToString());
	}
	auto &colref = expr.Cast<ColumnRefExpression>();
	if (colref.IsQualified()) {
		throw InvalidInputException("Unexpected qualified column reference - only columns are supported");
	}
	DuckLakePartitionField field;
	auto &col = table.GetColumn(colref.GetColumnName());
	field.column_id = col.Oid();
	field.transform.type = DuckLakeTransformType::IDENTITY;
	return field;
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, SetPartitionedByInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	// create a complete copy of this table with the partition info added
	auto partition_data = make_uniq<DuckLakePartition>();
	partition_data->partition_id = transaction.GetLocalCatalogId();
	for (idx_t expr_idx = 0; expr_idx < info.partition_keys.size(); expr_idx++) {
		auto &expr = *info.partition_keys[expr_idx];
		auto partition_field = GetPartitionField(*this, expr);
		partition_field.partition_key_index = expr_idx;
		partition_data->fields.push_back(partition_field);
	}

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info, std::move(partition_data));
	return std::move(new_entry);
}

optional_idx FindNotNullConstraint(CreateTableInfo &table_info, LogicalIndex index) {
	for (idx_t constraint_idx = 0; constraint_idx < table_info.constraints.size(); constraint_idx++) {
		auto &constraint = table_info.constraints[constraint_idx];
		if (constraint->type != ConstraintType::NOT_NULL) {
			continue;
		}
		auto &not_null_constraint = constraint->Cast<NotNullConstraint>();
		if (not_null_constraint.index == index) {
			return constraint_idx;
		}
	}
	return optional_idx();
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, SetNotNullInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	auto &col = table_info.columns.GetColumn(info.column_name);
	auto &field_id = GetFieldId(col.Physical());

	// verify the column has no NULL values currently by looking at the stats
	auto stats = GetTableStats(transaction);
	if (!stats) {
		throw CatalogException(
		    "Cannot SET NULL on table %s - the table has transaction-local changes or no stats are available", name);
	}

	auto column_stats = stats->column_stats.find(field_id.GetFieldIndex());
	if (column_stats == stats->column_stats.end()) {
		throw CatalogException("Cannot SET NULL on table %s - no column stats are available", name);
	}
	auto &col_stats = column_stats->second;
	if (col_stats.has_null_count && col_stats.null_count > 0) {
		throw CatalogException("Cannot SET NULL on column %s - the column has NULL values", col.GetName());
	}

	// check if there is an existing constraint
	auto existing_idx = FindNotNullConstraint(table_info, col.Logical());
	if (existing_idx.IsValid()) {
		throw CatalogException("Cannot SET NULL on column %s - it already has a NOT NULL constraint", col.GetName());
	}
	table_info.constraints.push_back(make_uniq<NotNullConstraint>(col.Logical()));

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChange::SetNull(field_id.GetFieldIndex()));
	return std::move(new_entry);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, DropNotNullInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	auto &col = table_info.columns.GetColumn(info.column_name);
	auto &field_id = GetFieldId(col.Physical());

	// find the existing index
	auto existing_idx = FindNotNullConstraint(table_info, col.Logical());
	if (!existing_idx.IsValid()) {
		throw CatalogException("Cannot DROP NULL on column %s - it has no NOT NULL constraint defined", col.GetName());
	}
	table_info.constraints.erase(table_info.constraints.begin() + existing_idx.GetIndex());

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChange::DropNull(field_id.GetFieldIndex()));
	return std::move(new_entry);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, RenameColumnInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	auto &col = table_info.columns.GetColumn(info.old_name);
	auto &field_id = GetFieldId(col.Physical());

	// create a new list with the renamed column
	ColumnList new_columns;
	for (auto &col : columns.Logical()) {
		auto copy = col.Copy();
		if (copy.Name() == info.old_name) {
			copy.SetName(info.new_name);
		}
		new_columns.AddColumn(std::move(copy));
	}
	table_info.columns = std::move(new_columns);

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info,
	                                               LocalChange::RenameColumn(field_id.GetFieldIndex()), info.new_name);
	return std::move(new_entry);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, AddColumnInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	if (info.if_column_not_exists && ColumnExists(info.new_column.Name())) {
		return nullptr;
	}

	table_info.columns.AddColumn(std::move(info.new_column));

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChangeType::ADD_COLUMN);
	return std::move(new_entry);
}

void ColumnChangeInfo::DropField(const DuckLakeFieldId &field_id) {
	dropped_fields.push_back(field_id.GetFieldIndex());
	for (auto &child_id : field_id.Children()) {
		DropField(*child_id);
	}
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, RemoveColumnInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	if (!ColumnExists(info.removed_column)) {
		if (info.if_column_exists) {
			return nullptr;
		}
		throw CatalogException("Cannot drop column %s - it does not exist", info.removed_column);
	}

	auto &col = table_info.columns.GetColumn(info.removed_column);
	auto &field_id = GetFieldId(col.Physical());
	if (columns.LogicalColumnCount() == 1) {
		throw CatalogException("Cannot drop column: table only has one column remaining!");
	}

	// remove the column from the column list
	ColumnList new_columns;
	for (auto &col : columns.Logical()) {
		auto copy = col.Copy();
		if (copy.Name() == info.removed_column) {
			continue;
		}
		new_columns.AddColumn(std::move(copy));
	}
	table_info.columns = std::move(new_columns);

	auto change_info = make_uniq<ColumnChangeInfo>();
	change_info->DropField(field_id);

	auto new_entry =
	    make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChangeType::REMOVE_COLUMN, std::move(change_info));
	return std::move(new_entry);
}

static bool TypePromotionIsAllowedTinyint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return true;
	default:
		return false;
	}
}

static bool TypePromotionIsAllowedSmallint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return true;
	default:
		return false;
	}
}

static bool TypePromotionIsAllowedUTinyint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

static bool TypePromotionIsAllowedUSmallint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}
bool TypePromotionIsAllowed(const LogicalType &source, const LogicalType &target) {
	// FIXME: support DECIMAL, and DATE -> TIMESTAMP
	switch (source.id()) {
	case LogicalTypeId::TINYINT:
		return TypePromotionIsAllowedTinyint(target);
	case LogicalTypeId::SMALLINT:
		return TypePromotionIsAllowedSmallint(target);
	case LogicalTypeId::INTEGER:
		return target.id() == LogicalTypeId::BIGINT;
	case LogicalTypeId::BIGINT:
		return false;
	case LogicalTypeId::UTINYINT:
		return TypePromotionIsAllowedUTinyint(target);
	case LogicalTypeId::USMALLINT:
		return TypePromotionIsAllowedUSmallint(target);
	case LogicalTypeId::UINTEGER:
		return target.id() == LogicalTypeId::UBIGINT;
	case LogicalTypeId::UBIGINT:
		return false;
	case LogicalTypeId::FLOAT:
		return target.id() == LogicalTypeId::DOUBLE;
	default:
		return false;
	}
}

bool IsSimpleCast(const ParsedExpression &expr) {
	if (expr.type != ExpressionType::OPERATOR_CAST) {
		return false;
	}
	auto &cast = expr.Cast<CastExpression>();
	if (cast.child->type != ExpressionType::COLUMN_REF) {
		return false;
	}
	return true;
}

unique_ptr<DuckLakeFieldId> DuckLakeTableEntry::GetStructEvolution(const DuckLakeFieldId &source_id,
                                                                   const LogicalType &target, ColumnChangeInfo &result,
                                                                   optional_idx parent_idx) {
	auto &source_types = StructType::GetChildTypes(source_id.Type());
	auto &target_types = StructType::GetChildTypes(target);

	case_insensitive_map_t<idx_t> source_type_map;
	for (idx_t source_idx = 0; source_idx < source_types.size(); ++source_idx) {
		source_type_map[source_types[source_idx].first] = source_idx;
	}
	auto &source_children = source_id.Children();
	auto struct_idx = source_id.GetFieldIndex();

	vector<unique_ptr<DuckLakeFieldId>> children;
	// for each type in target_types, check if it is in source types
	for (idx_t target_idx = 0; target_idx < target_types.size(); ++target_idx) {
		auto &target_type = target_types[target_idx];
		auto entry = source_type_map.find(target_type.first);
		if (entry == source_type_map.end()) {
			// type not found - this is a new entry
			// first construct a new field id for this entry
			auto field_id =
			    DuckLakeFieldId::FieldIdFromType(target_type.first, target_type.second, next_column_id.index);

			// add the column to the list of "to-be-added" columns
			DuckLakeNewColumn new_col;
			new_col.column_info = ConvertColumn(target_type.first, target_type.second, *field_id);
			new_col.parent_idx = struct_idx.index;
			result.new_fields.push_back(std::move(new_col));
			children.push_back(std::move(field_id));
			continue;
		}
		auto source_idx = entry->second;

		// the name exists in both the source and target
		// recursively perform type promotion
		auto new_child_id = TypePromotion(*source_children[source_idx], target_type.second, result, struct_idx.index);

		children.push_back(std::move(new_child_id));
		// erase from the source map to indicate this field has been handled
		source_type_map.erase(target_type.first);
	}
	for (auto &entry : source_type_map) {
		auto source_idx = entry.second;
		auto &source_field = *source_children[source_idx];
		result.DropField(source_field);
	}
	return make_uniq<DuckLakeFieldId>(struct_idx, source_id.Name(), target, std::move(children));
}

unique_ptr<DuckLakeFieldId> DuckLakeTableEntry::TypePromotion(const DuckLakeFieldId &source_id,
                                                              const LogicalType &target, ColumnChangeInfo &result,
                                                              optional_idx parent_idx) {
	auto &source_type = source_id.Type();
	if (source_type.id() == LogicalTypeId::STRUCT && target.id() == LogicalTypeId::STRUCT) {
		// both types are structs - perform struct type evolution
		return GetStructEvolution(source_id, target, result, parent_idx);
	}
	if (source_type == target) {
		// type is unchanged - return field id directly
		return source_id.Copy();
	}
	if (!source_id.Children().empty()) {
		throw NotImplementedException("Unsupported type evolution on nested field");
	}
	// primitive type promotion
	// only widening type promotions are allowed
	if (!TypePromotionIsAllowed(source_type, target)) {
		throw CatalogException(
		    "Cannot change type of column %s from %s to %s - only widening type promotions are allowed",
		    source_id.Name(), source_type, target);
	}
	// field id is unchanged - but the column is changed
	// we need to drop and recreate the column
	// drop the field
	result.DropField(source_id);

	// re-create with the new type
	auto field_idx = source_id.GetFieldIndex();
	DuckLakeNewColumn new_col;
	if (!parent_idx.IsValid()) {
		// root column - get the info from the table directly
		new_col.column_info = GetColumnInfo(field_idx);
	} else {
		// nested column - generate the info here
		new_col.column_info.id = field_idx;
		new_col.column_info.name = source_id.Name();
	}
	new_col.column_info.type = DuckLakeTypes::ToString(target);
	new_col.parent_idx = parent_idx;
	result.new_fields.push_back(std::move(new_col));

	return make_uniq<DuckLakeFieldId>(field_idx, source_id.Name(), target);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::AlterTable(DuckLakeTransaction &transaction, ChangeColumnTypeInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	if (!ColumnExists(info.column_name)) {
		throw CatalogException("Cannot change type of column %s - it does not exist", info.column_name);
	}
	auto &col = table_info.columns.GetColumn(info.column_name);
	auto &field_id = GetFieldId(col.Physical());
	if (!IsSimpleCast(*info.expression)) {
		throw NotImplementedException("Column type cannot be modified using an expression");
	}
	auto change_info = make_uniq<ColumnChangeInfo>();
	auto new_field_id = TypePromotion(field_id, info.target_type, *change_info, optional_idx());

	// generate a new column list with the modified type
	ColumnList new_columns;
	for (auto &col : columns.Logical()) {
		auto copy = col.Copy();
		if (copy.Name() == info.column_name) {
			copy.SetType(info.target_type);
		}
		new_columns.AddColumn(std::move(copy));
	}
	table_info.columns = std::move(new_columns);

	// generate the new field ids for the table
	auto &current_field_ids = field_data->GetFieldIds();
	auto new_field_ids = make_shared_ptr<DuckLakeFieldData>();
	for (auto &field_id : current_field_ids) {
		if (field_id->Name() == info.column_name) {
			new_field_ids->Add(std::move(new_field_id));
		} else {
			new_field_ids->Add(field_id->Copy());
		}
	}

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChangeType::CHANGE_COLUMN_TYPE,
	                                               std::move(change_info), std::move(new_field_ids));
	return std::move(new_entry);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::Alter(DuckLakeTransaction &transaction, AlterTableInfo &info) {
	switch (info.alter_table_type) {
	case AlterTableType::RENAME_TABLE:
		return AlterTable(transaction, info.Cast<RenameTableInfo>());
	case AlterTableType::SET_PARTITIONED_BY:
		return AlterTable(transaction, info.Cast<SetPartitionedByInfo>());
	case AlterTableType::SET_NOT_NULL:
		return AlterTable(transaction, info.Cast<SetNotNullInfo>());
	case AlterTableType::DROP_NOT_NULL:
		return AlterTable(transaction, info.Cast<DropNotNullInfo>());
	case AlterTableType::RENAME_COLUMN:
		return AlterTable(transaction, info.Cast<RenameColumnInfo>());
	case AlterTableType::ADD_COLUMN:
		return AlterTable(transaction, info.Cast<AddColumnInfo>());
	case AlterTableType::REMOVE_COLUMN:
		return AlterTable(transaction, info.Cast<RemoveColumnInfo>());
	case AlterTableType::ALTER_COLUMN_TYPE:
		return AlterTable(transaction, info.Cast<ChangeColumnTypeInfo>());
	default:
		throw BinderException("Unsupported ALTER TABLE type in DuckLake");
	}
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::Alter(DuckLakeTransaction &transaction, SetCommentInfo &info) {
	auto create_info = GetInfo();
	create_info->comment = info.comment_value;
	auto &table_info = create_info->Cast<CreateTableInfo>();

	auto new_entry = make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChangeType::SET_COMMENT);
	return std::move(new_entry);
}

unique_ptr<CatalogEntry> DuckLakeTableEntry::Alter(DuckLakeTransaction &transaction, SetColumnCommentInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	auto &col = table_info.columns.GetColumnMutable(info.column_name);
	col.SetComment(info.comment_value);
	auto &field_id = GetFieldId(col.Physical());

	auto new_entry =
	    make_uniq<DuckLakeTableEntry>(*this, table_info, LocalChange::SetColumnComment(field_id.GetFieldIndex()));
	return std::move(new_entry);
}

DuckLakeColumnInfo DuckLakeTableEntry::GetColumnInfo(FieldIndex field_index) const {
	auto &col = GetColumnByFieldId(field_index);

	DuckLakeColumnInfo result;
	result.id = field_index;
	result.name = col.Name();
	result.type = DuckLakeTypes::ToString(col.Type());
	result.nulls_allowed = GetNotNullFields().count(col.Name()) == 0;
	return result;
}

DuckLakeColumnInfo DuckLakeTableEntry::ConvertColumn(const string &name, const LogicalType &type,
                                                     const DuckLakeFieldId &field_id) {
	DuckLakeColumnInfo column_entry;
	column_entry.id = field_id.GetFieldIndex();
	column_entry.name = name;
	column_entry.nulls_allowed = true;
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		column_entry.type = "struct";
		auto &struct_children = StructType::GetChildTypes(type);
		for (idx_t child_idx = 0; child_idx < struct_children.size(); ++child_idx) {
			auto &child = struct_children[child_idx];
			auto &child_id = field_id.GetChildByIndex(child_idx);
			column_entry.children.push_back(ConvertColumn(child.first, child.second, child_id));
		}
		break;
	}
	case LogicalTypeId::LIST: {
		column_entry.type = "list";
		auto &child_id = field_id.GetChildByIndex(0);
		column_entry.children.push_back(ConvertColumn("element", ListType::GetChildType(type), child_id));
		break;
	}
	case LogicalTypeId::ARRAY: {
		column_entry.type = "list";
		auto &child_id = field_id.GetChildByIndex(0);
		column_entry.children.push_back(ConvertColumn("element", ArrayType::GetChildType(type), child_id));
		break;
	}
	case LogicalTypeId::MAP: {
		column_entry.type = "map";
		auto &key_id = field_id.GetChildByIndex(0);
		auto &value_id = field_id.GetChildByIndex(1);
		column_entry.children.push_back(ConvertColumn("key", MapType::KeyType(type), key_id));
		column_entry.children.push_back(ConvertColumn("value", MapType::ValueType(type), value_id));
		break;
	}
	default:
		column_entry.type = DuckLakeTypes::ToString(type);
		break;
	}
	return column_entry;
}

DuckLakeColumnInfo DuckLakeTableEntry::GetAddColumnInfo() const {
	// the column that is added is always the last column
	LogicalIndex new_col_idx(columns.LogicalColumnCount() - 1);
	auto &new_col = GetColumn(new_col_idx);

	auto &field_id = field_data->GetByRootIndex(new_col.Physical());
	return ConvertColumn(new_col.Name(), new_col.Type(), field_id);
}

TableStorageInfo DuckLakeTableEntry::GetStorageInfo(ClientContext &context) {
	return TableStorageInfo();
}

void DuckLakeTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                               LogicalUpdate &update, ClientContext &context) {
	throw InternalException("Unsupported function BindUpdateConstraints for table entry");
}

} // namespace duckdb
