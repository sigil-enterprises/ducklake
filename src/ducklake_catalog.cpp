#include "ducklake_catalog.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/main/attached_database.hpp"
#include "ducklake_initializer.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "ducklake_schema_entry.hpp"
#include "ducklake_table_entry.hpp"
#include "ducklake_transaction.hpp"

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
	throw InternalException("Unsupported DuckLake function");
}

void DuckLakeCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	auto &duck_transaction = DuckLakeTransaction::Get(context, *this);
	auto snapshot = duck_transaction.GetSnapshot();
	auto &schemas = GetSchemaForSnapshot(duck_transaction, snapshot);
	for (auto &schema : schemas.GetEntries()) {
		callback(schema.second->Cast<SchemaCatalogEntry>());
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

unique_ptr<DuckLakeCatalogSet> DuckLakeCatalog::LoadSchemaForSnapshot(DuckLakeTransaction &transaction,
                                                                      DuckLakeSnapshot snapshot) {
	auto result = transaction.Query(snapshot, R"(
SELECT schema_id, schema_uuid::VARCHAR, schema_name
FROM {METADATA_CATALOG}.ducklake_schema
WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get schema information from DuckLake: ");
	}

	ducklake_entries_map_t schema_map;
	unordered_map<idx_t, reference<DuckLakeSchemaEntry>> schema_id_map;
	for (auto &row : *result) {
		auto schema_id = row.GetValue<uint64_t>(0);
		auto schema_uuid = row.GetValue<string>(1);
		auto schema_name = row.GetValue<string>(2);

		CreateSchemaInfo schema_info;
		schema_info.schema = schema_name;
		auto schema_entry = make_uniq<DuckLakeSchemaEntry>(*this, schema_info, schema_id, std::move(schema_uuid));
		schema_id_map.insert(make_pair(schema_id, reference<DuckLakeSchemaEntry>(*schema_entry)));
		schema_map.insert(make_pair(std::move(schema_name), std::move(schema_entry)));
	}
	result = transaction.Query(snapshot, R"(
SELECT schema_id, table_id, table_uuid::VARCHAR, table_name, column_id, column_name, column_type, default_value
FROM {METADATA_CATALOG}.ducklake_table tbl
LEFT JOIN {METADATA_CATALOG}.ducklake_column col USING (table_id)
WHERE {SNAPSHOT_ID} >= tbl.begin_snapshot AND ({SNAPSHOT_ID} < tbl.end_snapshot OR tbl.end_snapshot IS NULL)
  AND (({SNAPSHOT_ID} >= col.begin_snapshot AND ({SNAPSHOT_ID} < col.end_snapshot OR col.end_snapshot IS NULL)) OR column_id IS NULL)
ORDER BY table_id, column_order
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get table information from DuckLake: ");
	}

	struct LoadedTableEntry {
		unique_ptr<CreateTableInfo> create_table_info;
		idx_t table_id;
		string table_uuid;
		optional_ptr<DuckLakeSchemaEntry> schema_entry;
	};

	vector<LoadedTableEntry> loaded_tables;
	for (auto &row : *result) {
		auto table_id = row.GetValue<uint64_t>(1);
		auto table_name = row.GetValue<string>(3);
		if (row.GetValue<Value>(4).IsNull()) {
			throw InvalidInputException("Failed to load DuckLake - Table entry \"%s\" does not have any columns",
			                            table_name);
		}
		//		auto column_id = row.GetValue<uint64_t>(4);
		auto column_name = row.GetValue<string>(5);
		auto column_type_str = row.GetValue<string>(6);
		//		auto default_value = row.GetValue<string>(7);

		// check if this column belongs to the current table or not
		if (loaded_tables.empty() || loaded_tables.back().table_id != table_id) {
			// new table
			auto schema_id = row.GetValue<uint64_t>(0);
			auto table_uuid = row.GetValue<string>(2);
			// find the schema
			auto entry = schema_id_map.find(schema_id);
			if (entry == schema_id_map.end()) {
				throw InvalidInputException(
				    "Failed to load DuckLake - could not find schema that corresponds to the table entry \"%s\"",
				    table_name);
			}
			LoadedTableEntry new_entry;
			new_entry.schema_entry = entry->second.get();
			new_entry.create_table_info = make_uniq<CreateTableInfo>(*new_entry.schema_entry, table_name);
			new_entry.table_id = table_id;
			new_entry.table_uuid = table_uuid;
			loaded_tables.push_back(std::move(new_entry));
		}
		auto &table_entry = loaded_tables.back();
		// add the column to this table
		auto column_type = DBConfig::ParseLogicalType(column_type_str);
		ColumnDefinition column(std::move(column_name), std::move(column_type));
		table_entry.create_table_info->columns.AddColumn(std::move(column));
		// FIXME: parse default value
		// FIXME: we need to keep the column id somehow
	}
	// flush the tables
	for (auto &entry : loaded_tables) {
		// flush the table
		auto table_entry = make_uniq<DuckLakeTableEntry>(*this, *entry.schema_entry, *entry.create_table_info,
		                                                 entry.table_id, std::move(entry.table_uuid));
		entry.schema_entry->AddEntry(CatalogType::TABLE_ENTRY, std::move(table_entry));
	}

	auto schema_set = make_uniq<DuckLakeCatalogSet>(CatalogType::SCHEMA_ENTRY, std::move(schema_map));
	return schema_set;
}

optional_ptr<SchemaCatalogEntry> DuckLakeCatalog::GetSchema(CatalogTransaction transaction, const string &schema_name,
                                                            OnEntryNotFound if_not_found,
                                                            QueryErrorContext error_context) {
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	auto snapshot = duck_transaction.GetSnapshot();
	auto &schemas = GetSchemaForSnapshot(duck_transaction, snapshot);
	auto entry = schemas.GetEntry<SchemaCatalogEntry>(duck_transaction, schema_name);
	if (!entry && if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw BinderException("DuckLakeCatalog - schema %s not found", schema_name);
	}
	return entry;
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanDelete(ClientContext &context, LogicalDelete &op,
                                                         unique_ptr<PhysicalOperator> plan) {
	throw InternalException("Unsupported DuckLake function");
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

void DuckLakeCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw InternalException("Unsupported DuckLake function");
}

string DuckLakeCatalog::GetDBPath() {
	return metadata_path;
}

} // namespace duckdb
