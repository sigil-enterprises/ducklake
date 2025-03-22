#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_view_entry.hpp"

#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {

DuckLakeSchemaEntry::DuckLakeSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, SchemaIndex schema_id,
                                         string schema_uuid)
    : SchemaCatalogEntry(catalog, info), schema_id(schema_id), schema_uuid(std::move(schema_uuid)) {
}

bool DuckLakeSchemaEntry::HandleCreateConflict(CatalogTransaction transaction, CatalogType catalog_type,
                                               const string &entry_name, OnCreateConflict on_conflict) {
	auto existing_entry = GetEntry(transaction, catalog_type, entry_name);
	if (!existing_entry) {
		// no conflict
		return true;
	}
	switch (on_conflict) {
	case OnCreateConflict::ERROR_ON_CONFLICT:
		throw CatalogException("%s with name \"%s\" already exists", CatalogTypeToString(existing_entry->type),
		                       entry_name);
	case OnCreateConflict::IGNORE_ON_CONFLICT:
		// ignore - skip without throwing an error
		return false;
	case OnCreateConflict::REPLACE_ON_CONFLICT: {
		// try to drop the entry prior to creating
		DropInfo info;
		info.type = catalog_type;
		info.name = entry_name;
		DropEntry(transaction.GetContext(), info);
		break;
	}
	default:
		throw InternalException("Unsupported conflict type");
	}
	return true;
}

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
	auto &base_info = info.Base();
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	// check if we have an existing entry with this name
	if (!HandleCreateConflict(transaction, CatalogType::TABLE_ENTRY, base_info.table, base_info.on_conflict)) {
		return nullptr;
	}
	//! get a local table-id
	auto table_id = TableIndex(duck_transaction.GetLocalCatalogId());
	auto table_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	// generate field ids based on the column ids
	auto field_data = DuckLakeFieldData::FromColumns(base_info.columns);
	auto table_entry = make_uniq<DuckLakeTableEntry>(ParentCatalog(), *this, base_info, table_id, std::move(table_uuid),
	                                                 std::move(field_data), TransactionLocalChange::CREATED);
	auto result = table_entry.get();
	duck_transaction.CreateEntry(std::move(table_entry));
	return result;
}

bool DuckLakeSchemaEntry::CatalogTypeIsSupported(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
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
	// check if we have an existing entry with this name
	if (!HandleCreateConflict(transaction, CatalogType::VIEW_ENTRY, info.view_name, info.on_conflict)) {
		return nullptr;
	}
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	//! get a local view-id
	auto view_id = TableIndex(duck_transaction.GetLocalCatalogId());
	auto view_uuid = UUID::ToString(UUID::GenerateRandomUUID());

	auto view_entry = make_uniq<DuckLakeViewEntry>(ParentCatalog(), *this, info, view_id, std::move(view_uuid),
	                                               info.query->ToString(), TransactionLocalChange::CREATED);
	auto result = view_entry.get();
	duck_transaction.CreateEntry(std::move(view_entry));
	return result;
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

void DuckLakeSchemaEntry::Alter(CatalogTransaction catalog_transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_TABLE) {
		throw BinderException("Only altering tables is supported for now");
	}
	auto &alter = info.Cast<AlterTableInfo>();
	auto &transaction = DuckLakeTransaction::Get(catalog_transaction.GetContext(), catalog);
	auto table_entry = GetEntry(catalog_transaction, CatalogType::TABLE_ENTRY, alter.name);
	auto &table = table_entry->Cast<DuckLakeTableEntry>();
	auto new_table = table.Alter(transaction, alter);
	transaction.AlterEntry(table, std::move(new_table));
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
		if (duck_transaction.IsDeleted(*entry.second)) {
			continue;
		}
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
	if (info.cascade) {
		throw NotImplementedException("Cascade Drop not supported in DuckLake");
	}
	auto catalog_entry = GetEntry(GetCatalogTransaction(context), info.type, info.name);
	if (!catalog_entry) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw InternalException("Failed to drop entry \"%s\" - could not find entry", info.name);
	}
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	transaction.DropEntry(*catalog_entry);
}

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	auto catalog_type = lookup_info.GetCatalogType();
	auto &entry_name = lookup_info.GetEntryName();
	if (!CatalogTypeIsSupported(catalog_type)) {
		return nullptr;
	}
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	//! search in transaction local storage first
	auto transaction_entry = duck_transaction.GetTransactionLocalEntry(catalog_type, name, entry_name);
	if (transaction_entry) {
		return transaction_entry;
	}
	auto &catalog_set = GetCatalogSet(catalog_type);
	auto entry = catalog_set.GetEntry(entry_name);
	if (!entry) {
		return nullptr;
	}
	if (duck_transaction.IsDeleted(*entry)) {
		return nullptr;
	}
	return *entry;
}

void DuckLakeSchemaEntry::AddEntry(CatalogType type, unique_ptr<CatalogEntry> entry) {
	auto &catalog_set = GetCatalogSet(type);
	catalog_set.CreateEntry(std::move(entry));
}

void DuckLakeSchemaEntry::TryDropSchema(DuckLakeTransaction &transaction, bool cascade) {
	if (!cascade) {
		// get a list of all dependents
		vector<reference<CatalogEntry>> dependents;
		for (auto &entry : tables.GetEntries()) {
			dependents.push_back(*entry.second);
		}
		if (dependents.empty()) {
			return;
		}
		string error_string = "Cannot drop schema \"" + name + "\" because there are entries that depend on it\n";
		for (auto &dependent : dependents) {
			auto &dep = dependent.get();
			error_string += StringUtil::Format("%s %s depends on %s.\n", CatalogTypeToString(dep.type), dep.name, name);
		}
		error_string += "Use DROP...CASCADE to drop all dependents.";
		throw CatalogException(error_string);
	}
	// drop all dependents
	for (auto &entry : tables.GetEntries()) {
		transaction.DropEntry(*entry.second);
	}
}

DuckLakeCatalogSet &DuckLakeSchemaEntry::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return tables;
	default:
		throw NotImplementedException("Unimplemented catalog type for schema");
	}
}

} // namespace duckdb
