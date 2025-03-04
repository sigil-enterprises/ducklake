#include "ducklake_catalog.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/main/attached_database.hpp"
#include "ducklake_initializer.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "ducklake_schema_entry.hpp"

namespace duckdb {

DuckLakeCatalog::DuckLakeCatalog(AttachedDatabase &db_p, string metadata_database_p, string metadata_path_p, string data_path_p, string metadata_schema_p)
    : Catalog(db_p), metadata_database(std::move(metadata_database_p)), metadata_path(std::move(metadata_path_p)), data_path(std::move(data_path_p)), metadata_schema(std::move(metadata_schema_p)) {
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
	for(auto &schema : schemas.GetEntries()) {
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

unique_ptr<DuckLakeCatalogSet> DuckLakeCatalog::LoadSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot) {
	auto result = transaction.Query(snapshot, R"(
SELECT schema_id, schema_uuid::VARCHAR, schema_name
FROM {METADATA_CATALOG}.ducklake_schema
WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get schema information from DuckLake: ");
	}

	ducklake_entries_map_t schema_map;
	for(auto &row : *result) {
		auto schema_id = row.GetValue<uint64_t>(0);
		auto schema_uuid = row.GetValue<string>(1);
		auto schema_name = row.GetValue<string>(2);

		CreateSchemaInfo schema_info;
		schema_info.schema = schema_name;
		auto schema_entry = make_uniq<DuckLakeSchemaEntry>(*this, schema_info, schema_id, std::move(schema_uuid));
		schema_map.insert(make_pair(std::move(schema_name), std::move(schema_entry)));
	}
	auto schema_set = make_uniq<DuckLakeCatalogSet>(std::move(schema_map));
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

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanInsert(ClientContext &context, LogicalInsert &op,
                                                         unique_ptr<PhysicalOperator> plan) {
	throw InternalException("Unsupported DuckLake function");
}
unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanCreateTableAs(ClientContext &context, LogicalCreateTable &op,
                                                                unique_ptr<PhysicalOperator> plan) {
	throw InternalException("Unsupported DuckLake function");
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
