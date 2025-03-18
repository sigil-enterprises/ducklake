#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_initializer.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_types.hpp"

#include "duckdb/storage/database_size.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/common/types/uuid.hpp"

namespace duckdb {

DuckLakeCatalog::DuckLakeCatalog(AttachedDatabase &db_p, string metadata_database_p, string metadata_path_p,
                                 string data_path_p, string metadata_schema_p)
    : Catalog(db_p), metadata_database(std::move(metadata_database_p)), metadata_path(std::move(metadata_path_p)),
      data_path(std::move(data_path_p)), metadata_schema(std::move(metadata_schema_p)) {
}

DuckLakeCatalog::~DuckLakeCatalog() {
}

void DuckLakeCatalog::Initialize(bool load_builtin) {
	throw InternalException("DuckLakeCatalog cannot be initialized without a client context");
}

void DuckLakeCatalog::Initialize(optional_ptr<ClientContext> context, bool load_builtin) {
	// initialize the metadata database
	DuckLakeInitializer initializer(*context, *this, metadata_database, metadata_path, metadata_schema, data_path);
	initializer.Initialize();
}

optional_ptr<CatalogEntry> DuckLakeCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	auto schema = GetSchema(transaction, info.schema, OnEntryNotFound::RETURN_NULL);
	if (schema) {
		if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			return nullptr;
		}
		if (info.on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			return nullptr;
		}
		// drop the existing entry
		DropInfo drop_info;
		drop_info.type = CatalogType::SCHEMA_ENTRY;
		drop_info.name = info.schema;
		DropSchema(transaction.GetContext(), drop_info);
	}
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	//! get a local table-id
	idx_t schema_id = duck_transaction.GetLocalCatalogId();
	auto schema_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	auto schema_entry = make_uniq<DuckLakeSchemaEntry>(*this, info, schema_id, std::move(schema_uuid));
	auto result = schema_entry.get();
	duck_transaction.CreateEntry(std::move(schema_entry));
	return result;
}

void DuckLakeCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	auto schema = GetSchema(GetCatalogTransaction(context), info.name, info.if_not_found);
	if (!schema) {
		return;
	}
	auto &transaction = DuckLakeTransaction::Get(context, *this);
	auto &ducklake_schema = schema->Cast<DuckLakeSchemaEntry>();
	ducklake_schema.TryDropSchema(transaction, info.cascade);
	transaction.DropEntry(*schema);
}

void DuckLakeCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	auto &duck_transaction = DuckLakeTransaction::Get(context, *this);
	auto set = duck_transaction.GetTransactionLocalSchemas();
	if (set) {
		for (auto &entry : set->GetEntries()) {
			callback(entry.second->Cast<SchemaCatalogEntry>());
		}
	}
	auto snapshot = duck_transaction.GetSnapshot();
	auto &schemas = GetSchemaForSnapshot(duck_transaction, snapshot);
	for (auto &schema : schemas.GetEntries()) {
		auto &schema_entry = schema.second->Cast<SchemaCatalogEntry>();
		if (duck_transaction.IsDeleted(schema_entry)) {
			continue;
		}
		callback(schema_entry);
	}
}

DuckLakeCatalogSet &DuckLakeCatalog::GetSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot) {
	lock_guard<mutex> guard(schemas_lock);
	auto entry = schemas.find(snapshot.schema_version);
	if (entry != schemas.end()) {
		// this schema version is already cached
		return *entry->second;
	}
	// load the schema version from the metadata manager
	auto schema = LoadSchemaForSnapshot(transaction, snapshot);
	auto &result = *schema;
	schemas.insert(make_pair(snapshot.schema_version, std::move(schema)));
	return result;
}

LogicalType DuckLakeCatalog::ParseDuckLakeType(const string &type) {
	if (StringUtil::EndsWith(type, "[]")) {
		// list - recurse
		auto child_type = ParseDuckLakeType(type.substr(0, type.size() - 2));
		return LogicalType::LIST(child_type);
	}

	if (StringUtil::StartsWith(type, "MAP(") && StringUtil::EndsWith(type, ")")) {
		// map - recurse
		string map_args = type.substr(4, type.size() - 5);
		vector<string> map_args_vect = StringUtil::SplitWithParentheses(map_args);
		if (map_args_vect.size() != 2) {
			throw InvalidInputException("Ill formatted map type: '%s'", type);
		}
		StringUtil::Trim(map_args_vect[0]);
		StringUtil::Trim(map_args_vect[1]);
		auto key_type = ParseDuckLakeType(map_args_vect[0]);
		auto value_type = ParseDuckLakeType(map_args_vect[1]);
		return LogicalType::MAP(key_type, value_type);
	}

	if (StringUtil::StartsWith(type, "STRUCT(") && StringUtil::EndsWith(type, ")")) {
		// struct - recurse
		string struct_members_str = type.substr(7, type.size() - 8);
		vector<string> struct_members_vect = StringUtil::SplitWithParentheses(struct_members_str);
		child_list_t<LogicalType> struct_members;
		for (idx_t member_idx = 0; member_idx < struct_members_vect.size(); member_idx++) {
			StringUtil::Trim(struct_members_vect[member_idx]);
			vector<string> struct_member_parts = StringUtil::SplitWithParentheses(struct_members_vect[member_idx], ' ');
			if (struct_member_parts.size() != 2) {
				throw InvalidInputException("Ill formatted struct type: %s", type);
			}
			StringUtil::Trim(struct_member_parts[0]);
			StringUtil::Trim(struct_member_parts[1]);
			auto value_type = ParseDuckLakeType(struct_member_parts[1]);
			struct_members.emplace_back(make_pair(struct_member_parts[0], value_type));
		}
		return LogicalType::STRUCT(struct_members);
	}
	if (StringUtil::StartsWith(type, "DECIMAL(") && StringUtil::EndsWith(type, ")")) {
		// decimal - parse width/scale
		string decimal_members_str = type.substr(8, type.size() - 9);
		vector<string> decimal_members_vect = StringUtil::SplitWithParentheses(decimal_members_str);
		if (decimal_members_vect.size() != 2) {
			throw InvalidInputException("Invalid DECIMAL type - expected width and scale");
		}
		auto width = std::stoull(decimal_members_vect[0]);
		auto scale = std::stoull(decimal_members_vect[1]);
		return LogicalType::DECIMAL(width, scale);
	}

	LogicalType type_id = StringUtil::CIEquals(type, "ANY") ? LogicalType::ANY : TransformStringToLogicalTypeId(type);
	if (type_id == LogicalTypeId::USER) {
		throw InvalidInputException(
		    "Error while generating extension function overloads - unrecognized logical type %s", type);
	}
	return type_id;
}

LogicalType TransformColumnType(DuckLakeColumnInfo &col, DuckLakeFieldId &field_id) {
	field_id.id = col.id;
	if (col.children.empty()) {
		return DuckLakeTypes::FromString(col.type);
	}
	if (StringUtil::CIEquals(col.type, "struct")) {
		child_list_t<LogicalType> child_types;
		for(auto &child_col : col.children) {
			DuckLakeFieldId child_id;
			child_types.emplace_back(make_pair(std::move(child_col.name), TransformColumnType(child_col, child_id)));
			field_id.children.push_back(std::move(child_id));
		}
		return LogicalType::STRUCT(std::move(child_types));
	}
	throw InvalidInputException("Unrecognized nested type \"%s\"", col.type);
}

unique_ptr<DuckLakeCatalogSet> DuckLakeCatalog::LoadSchemaForSnapshot(DuckLakeTransaction &transaction,
                                                                      DuckLakeSnapshot snapshot) {
	auto &metadata_manager = transaction.GetMetadataManager();
	auto catalog = metadata_manager.GetCatalogForSnapshot(snapshot);
	ducklake_entries_map_t schema_map;
	unordered_map<idx_t, reference<DuckLakeSchemaEntry>> schema_id_map;
	for (auto &schema : catalog.schemas) {
		CreateSchemaInfo schema_info;
		schema_info.schema = schema.name;
		auto schema_entry = make_uniq<DuckLakeSchemaEntry>(*this, schema_info, schema.id, std::move(schema.uuid));
		schema_id_map.insert(make_pair(schema.id, reference<DuckLakeSchemaEntry>(*schema_entry)));
		schema_map.insert(make_pair(std::move(schema.name), std::move(schema_entry)));
	}

	auto schema_set = make_uniq<DuckLakeCatalogSet>(std::move(schema_map));
	// load the table entries
	for (auto &table : catalog.tables) {
		// find the schema for the table
		auto entry = schema_id_map.find(table.schema_id);
		if (entry == schema_id_map.end()) {
			throw InvalidInputException(
			    "Failed to load DuckLake - could not find schema that corresponds to the table entry \"%s\"",
			    table.name);
		}
		auto &schema_entry = entry->second.get();
		auto create_table_info = make_uniq<CreateTableInfo>(schema_entry, table.name);
		// parse the columns
		vector<DuckLakeFieldId> field_ids;
		for (auto &col_info : table.columns) {
			DuckLakeFieldId field_id;
			auto column_type = TransformColumnType(col_info, field_id);
			ColumnDefinition column(std::move(col_info.name), std::move(column_type));
			create_table_info->columns.AddColumn(std::move(column));
			field_ids.push_back(std::move(field_id));
		}
		// create the table and add it to the schema set
		auto table_entry = make_uniq<DuckLakeTableEntry>(*this, schema_entry, *create_table_info, table.id,
		                                                 std::move(table.uuid), std::move(field_ids), TransactionLocalChange::NONE);
		schema_set->AddEntry(schema_entry, table.id, std::move(table_entry));
	}

	// load the partition entries
	for (auto &entry : catalog.partitions) {
		auto table = schema_set->GetEntryById(entry.table_id);
		if (!table || table->type != CatalogType::TABLE_ENTRY) {
			throw InvalidInputException("Could not find matching table for partition entry");
		}
		auto partition = make_uniq<DuckLakePartition>();
		partition->partition_id = entry.id.GetIndex();
		for (auto &field : entry.fields) {
			DuckLakePartitionField partition_field;
			partition_field.partition_key_index = field.partition_key_index;
			partition_field.column_id = field.column_id;
			partition_field.transform.type = DuckLakeTransformType::IDENTITY;
			partition->fields.push_back(std::move(partition_field));
		}
		auto &ducklake_table = table->Cast<DuckLakeTableEntry>();
		ducklake_table.SetPartitionData(std::move(partition));
	}
	return schema_set;
}

DuckLakeStats &DuckLakeCatalog::GetStatsForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot) {
	auto &schema = GetSchemaForSnapshot(transaction, snapshot);
	lock_guard<mutex> guard(schemas_lock);
	auto entry = stats.find(snapshot.next_file_id);
	if (entry != stats.end()) {
		// this stats are already cached
		return *entry->second;
	}
	// load the stats from the metadata manager
	auto table_stats = LoadStatsForSnapshot(transaction, snapshot, schema);
	auto &result = *table_stats;
	stats.insert(make_pair(snapshot.next_file_id, std::move(table_stats)));
	return result;
}

unique_ptr<DuckLakeStats> DuckLakeCatalog::LoadStatsForSnapshot(DuckLakeTransaction &transaction,
                                                                DuckLakeSnapshot snapshot, DuckLakeCatalogSet &schema) {
	auto &metadata_manager = transaction.GetMetadataManager();
	auto global_stats = metadata_manager.GetGlobalTableStats(snapshot);

	// construct the stats map
	auto lake_stats = make_uniq<DuckLakeStats>();
	for (auto &stats : global_stats) {
		// find the referenced table entry
		auto table_entry = schema.GetEntryById(stats.table_id);
		if (!table_entry) {
			// failed to find the referenced table entry - this means the table does not exist for this snapshot
			// since the global stats are not versioned this is not an error - just skip
			continue;
		}
		auto table_stats = make_uniq<DuckLakeTableStats>();
		table_stats->record_count = stats.record_count;
		table_stats->table_size_bytes = stats.table_size_bytes;
		auto &table = table_entry->Cast<DuckLakeTableEntry>();
		for (auto &col_stats : stats.column_stats) {
			// FIXME: this is wrong
			auto &col = table.GetColumn(LogicalIndex(col_stats.column_id.index));
			DuckLakeColumnStats column_stats(col.Type());
			column_stats.has_null_count = col_stats.has_contains_null;
			if (column_stats.has_null_count) {
				column_stats.null_count = col_stats.contains_null ? 1 : 0;
			}
			column_stats.has_min = col_stats.has_min;
			if (column_stats.has_min) {
				column_stats.min = col_stats.min_val;
			}
			column_stats.has_max = col_stats.has_max;
			if (column_stats.has_max) {
				column_stats.max = col_stats.max_val;
			}
			table_stats->column_stats.insert(make_pair(col_stats.column_id, std::move(column_stats)));
		}
		lake_stats->table_stats.insert(make_pair(stats.table_id, std::move(table_stats)));
	}
	return lake_stats;
}

optional_ptr<DuckLakeTableStats> DuckLakeCatalog::GetTableStats(DuckLakeTransaction &transaction, TableIndex table_id) {
	return GetTableStats(transaction, transaction.GetSnapshot(), table_id);
}

optional_ptr<DuckLakeTableStats> DuckLakeCatalog::GetTableStats(DuckLakeTransaction &transaction,
                                                                DuckLakeSnapshot snapshot, TableIndex table_id) {
	auto &lake_stats = GetStatsForSnapshot(transaction, snapshot);
	auto entry = lake_stats.table_stats.find(table_id);
	if (entry == lake_stats.table_stats.end()) {
		return nullptr;
	}
	return entry->second.get();
}

optional_ptr<SchemaCatalogEntry> DuckLakeCatalog::GetSchema(CatalogTransaction transaction, const string &schema_name,
                                                            OnEntryNotFound if_not_found,
                                                            QueryErrorContext error_context) {
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	// look for the schema in the set of transaction-local schemas
	auto set = duck_transaction.GetTransactionLocalSchemas();
	if (set) {
		auto entry = set->GetEntry<SchemaCatalogEntry>(schema_name);
		if (entry) {
			return entry;
		}
	}
	auto snapshot = duck_transaction.GetSnapshot();
	auto &schemas = GetSchemaForSnapshot(duck_transaction, snapshot);
	auto entry = schemas.GetEntry<SchemaCatalogEntry>(schema_name);
	if (!entry) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw BinderException("Schema \"%s\" not found in DuckLakeCatalog \"%s\"", schema_name, GetName());
		}
		return nullptr;
	}
	if (duck_transaction.IsDeleted(*entry)) {
		return nullptr;
	}
	return entry;
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanUpdate(ClientContext &context, LogicalUpdate &op,
                                                         unique_ptr<PhysicalOperator> plan) {
	throw InternalException("Unsupported DuckLake function");
}
unique_ptr<LogicalOperator> DuckLakeCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                             TableCatalogEntry &table,
                                                             unique_ptr<LogicalOperator> plan) {
	throw InternalException("Unsupported DuckLake function");
}

DatabaseSize DuckLakeCatalog::GetDatabaseSize(ClientContext &context) {
	throw InternalException("Unsupported DuckLake function");
}

bool DuckLakeCatalog::InMemory() {
	return false;
}

string DuckLakeCatalog::GetDBPath() {
	return metadata_path;
}

} // namespace duckdb
