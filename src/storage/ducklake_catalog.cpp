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
	for(auto &table : catalog.tables) {
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
		for(auto &col_info : table.columns) {
			auto column_type = DuckLakeTypes::FromString(col_info.type);
			ColumnDefinition column(std::move(col_info.name), std::move(column_type));
			create_table_info->columns.AddColumn(std::move(column));
		}
		// create the table and add it to the schema set
		auto table_entry =
			make_uniq<DuckLakeTableEntry>(*this, schema_entry, *create_table_info, table.id,
										  std::move(table.uuid), TransactionLocalChange::NONE);
		schema_set->AddEntry(schema_entry, table.id, std::move(table_entry));
	}

	// load partition information
	auto result = transaction.Query(snapshot, R"(
SELECT partition_id, table_id, partition_key_index, column_id, transform
FROM {METADATA_CATALOG}.ducklake_partition_info part
JOIN {METADATA_CATALOG}.ducklake_partition_columns part_col USING (partition_id)
WHERE {SNAPSHOT_ID} >= part.begin_snapshot AND ({SNAPSHOT_ID} < part.end_snapshot OR part.end_snapshot IS NULL)
ORDER BY table_id, partition_id, partition_key_index
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get partition information from DuckLake: ");
	}
	struct LoadedPartitionEntry {
		idx_t table_id;
		unique_ptr<DuckLakePartition> partition;
	};
	vector<LoadedPartitionEntry> loaded_partitions;
	for (auto &row : *result) {
		auto partition_id = row.GetValue<uint64_t>(0);
		auto table_id = row.GetValue<uint64_t>(1);

		if (loaded_partitions.empty() || loaded_partitions.back().table_id != table_id) {
			LoadedPartitionEntry entry;
			entry.table_id = table_id;
			entry.partition = make_uniq<DuckLakePartition>();
			entry.partition->partition_id = partition_id;
			loaded_partitions.push_back(std::move(entry));
		}
		auto &partition_entry = loaded_partitions.back();

		DuckLakePartitionField partition_field;
		partition_field.partition_key_index = row.GetValue<uint64_t>(2);
		partition_field.column_id = row.GetValue<uint64_t>(3);
		auto transform = row.GetValue<string>(4);
		if (transform != "identity") {
			throw NotImplementedException("Unsupported transform found in DuckLake: %s", transform);
		}
		partition_field.transform.type = DuckLakeTransformType::IDENTITY;
		partition_entry.partition->fields.push_back(std::move(partition_field));
	}
	// flush the partition entries
	for (auto &entry : loaded_partitions) {
		auto table = schema_set->GetEntryById(entry.table_id);
		if (!table || table->type != CatalogType::TABLE_ENTRY) {
			throw InvalidInputException("Could not find matching table for partition entry");
		}
		auto &ducklake_table = table->Cast<DuckLakeTableEntry>();
		ducklake_table.SetPartitionData(std::move(entry.partition));
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
	// query the most recent stats
	auto result = transaction.Query(snapshot, R"(
SELECT table_id, column_id, record_count, file_size_bytes, contains_null, min_value, max_value
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats USING (table_id)
WHERE record_count IS NOT NULL AND file_size_bytes IS NOT NULL
ORDER BY table_id;
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get global stats information from DuckLake: ");
	}
	struct LoadedStatsEntry {
		idx_t table_id;
		optional_ptr<DuckLakeTableEntry> table_entry;
		unique_ptr<DuckLakeTableStats> stats;
	};
	vector<LoadedStatsEntry> loaded_stats;
	for (auto &row : *result) {
		auto table_id = row.GetValue<uint64_t>(0);
		auto column_id = row.GetValue<uint64_t>(1);

		if (loaded_stats.empty() || loaded_stats.back().table_id != table_id) {
			// new stats
			LoadedStatsEntry new_entry;

			// find the referenced table entry
			auto table_entry = schema.GetEntryById(table_id);
			if (table_entry && table_entry->type == CatalogType::TABLE_ENTRY) {
				new_entry.table_entry = table_entry->Cast<DuckLakeTableEntry>();
			}

			// set up the table-level stats
			new_entry.table_id = table_id;
			new_entry.stats = make_uniq<DuckLakeTableStats>();
			new_entry.stats->record_count = row.GetValue<uint64_t>(2);
			new_entry.stats->table_size_bytes = row.GetValue<uint64_t>(3);
			loaded_stats.push_back(std::move(new_entry));
		}
		// add the stats for this column
		auto &stats_entry = loaded_stats.back();
		if (!stats_entry.table_entry) {
			continue;
		}
		auto &col = stats_entry.table_entry->GetColumn(LogicalIndex(column_id));
		DuckLakeColumnStats column_stats(col.Type());
		if (row.IsNull(4)) {
			column_stats.has_null_count = false;
		} else {
			column_stats.has_null_count = true;
			column_stats.null_count = row.GetValue<uint64_t>(4);
		}
		if (row.IsNull(5)) {
			column_stats.has_min = false;
		} else {
			column_stats.has_min = true;
			column_stats.min = row.GetValue<string>(5);
		}
		if (row.IsNull(6)) {
			column_stats.has_max = false;
		} else {
			column_stats.has_max = true;
			column_stats.max = row.GetValue<string>(6);
		}
		stats_entry.stats->column_stats.insert(make_pair(column_id, std::move(column_stats)));
	}
	// construct the stats map
	auto lake_stats = make_uniq<DuckLakeStats>();
	for (auto &stats : loaded_stats) {
		lake_stats->table_stats.insert(make_pair(stats.table_id, std::move(stats.stats)));
	}
	return lake_stats;
}

optional_ptr<DuckLakeTableStats> DuckLakeCatalog::GetTableStats(DuckLakeTransaction &transaction, idx_t table_id) {
	return GetTableStats(transaction, transaction.GetSnapshot(), table_id);
}

optional_ptr<DuckLakeTableStats> DuckLakeCatalog::GetTableStats(DuckLakeTransaction &transaction,
                                                                DuckLakeSnapshot snapshot, idx_t table_id) {
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
