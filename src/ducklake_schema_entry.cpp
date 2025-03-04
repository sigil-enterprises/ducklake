#include "ducklake_schema_entry.hpp"

namespace duckdb {

DuckLakeSchemaEntry::DuckLakeSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateFunction(CatalogTransaction transaction,
                                                               CreateFunctionInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                            TableCatalogEntry &table) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateSequence(CatalogTransaction transaction,
                                                               CreateSequenceInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                    CreateTableFunctionInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                   CreateCopyFunctionInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                     CreatePragmaFunctionInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                                CreateCollationInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	throw InternalException("Unsupported schema operation");
}
void DuckLakeSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	throw InternalException("Unsupported schema operation");
}
void DuckLakeSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	throw InternalException("Unsupported schema operation");
}
void DuckLakeSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw InternalException("Unsupported schema operation");
}
void DuckLakeSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	throw InternalException("Unsupported schema operation");
}
optional_ptr<CatalogEntry> DuckLakeSchemaEntry::GetEntry(CatalogTransaction transaction, CatalogType type,
                                                         const string &name) {
	throw InternalException("Unsupported schema operation");
}

} // namespace duckdb
