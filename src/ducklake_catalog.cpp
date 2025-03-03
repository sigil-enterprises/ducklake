#include "ducklake_catalog.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

DuckLakeCatalog::DuckLakeCatalog(AttachedDatabase &db_p, string metadata_database_p)
    : Catalog(db_p), metadata_database(std::move(metadata_database_p)) {
}

DuckLakeCatalog::~DuckLakeCatalog() {
}

void DuckLakeCatalog::Initialize(bool load_builtin) {
	throw InternalException("Unsupported DuckLake function");
}

optional_ptr<CatalogEntry> DuckLakeCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw InternalException("Unsupported DuckLake function");
}

void DuckLakeCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	throw InternalException("Unsupported DuckLake function");
}

optional_ptr<SchemaCatalogEntry> DuckLakeCatalog::GetSchema(CatalogTransaction transaction, const string &schema_name,
                                                            OnEntryNotFound if_not_found,
                                                            QueryErrorContext error_context) {
	throw InternalException("Unsupported DuckLake function");
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
	throw InternalException("Unsupported DuckLake function");
}

void DuckLakeCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw InternalException("Unsupported DuckLake function");
}

string DuckLakeCatalog::GetDBPath() {
	throw InternalException("Unsupported DuckLake function");
}

} // namespace duckdb
