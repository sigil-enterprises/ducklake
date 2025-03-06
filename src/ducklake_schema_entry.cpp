#include "ducklake_schema_entry.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "ducklake_table_entry.hpp"
#include "ducklake_transaction.hpp"
#include "duckdb/common/types/uuid.hpp"

namespace duckdb {

DuckLakeSchemaEntry::DuckLakeSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, idx_t schema_id, string schema_uuid)
    : SchemaCatalogEntry(catalog, info), schema_id(schema_id), schema_uuid(std::move(schema_uuid)) {
}

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	auto &entry = duck_transaction.GetOrCreateTransactionLocalEntries(CatalogType::TABLE_ENTRY, name);
	//! get a local table-id
	idx_t table_id = duck_transaction.GetLocalTableId();
	auto table_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	auto table_entry =
	    make_uniq<DuckLakeTableEntry>(ParentCatalog(), *this, info.Base(), table_id, std::move(table_uuid));
	auto result = table_entry.get();
	entry.CreateEntry(std::move(table_entry));
	return result;
}

bool DuckLakeSchemaEntry::CatalogTypeIsSupported(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return true;
	default:
		return false;
	}
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
	if (!CatalogTypeIsSupported(type)) {
		return;
	}
	// scan transaction-local entries
	auto &duck_transaction = DuckLakeTransaction::Get(context, ParentCatalog());
	auto set = duck_transaction.GetTransactionLocalEntries(type, name);
	if (set) {
		for (auto &entry : set->GetEntries()) {
			callback(*entry.second);
		}
	}
	// scan committed entries
	auto &catalog_set = GetCatalogSet(type);
	for (auto &entry : catalog_set.GetEntries()) {
		callback(*entry.second);
	}
}

void DuckLakeSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	auto &catalog_set = GetCatalogSet(type);
	for (auto &entry : catalog_set.GetEntries()) {
		callback(*entry.second);
	}
}
void DuckLakeSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	throw InternalException("Unsupported schema operation");
}

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::GetEntry(CatalogTransaction transaction, CatalogType type,
                                                         const string &entry_name) {
	if (!CatalogTypeIsSupported(type)) {
		return nullptr;
	}
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	//! search in transaction local storage first
	auto transaction_entry = duck_transaction.GetTransactionLocalEntry(type, name, entry_name);
	if (transaction_entry) {
		return transaction_entry;
	}
	auto &catalog_set = GetCatalogSet(type);
	return catalog_set.GetEntry(entry_name);
}

void DuckLakeSchemaEntry::AddEntry(CatalogType type, unique_ptr<CatalogEntry> entry) {
	auto &catalog_set = GetCatalogSet(type);
	catalog_set.CreateEntry(std::move(entry));
}

DuckLakeCatalogSet &DuckLakeSchemaEntry::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return tables;
	default:
		throw NotImplementedException("Unimplemented catalog type for schema");
	}
}

} // namespace duckdb
