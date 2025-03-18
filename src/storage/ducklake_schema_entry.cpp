#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"

#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {

DuckLakeSchemaEntry::DuckLakeSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, SchemaIndex schema_id, string schema_uuid)
    : SchemaCatalogEntry(catalog, info), schema_id(schema_id), schema_uuid(std::move(schema_uuid)) {
}

unique_ptr<DuckLakeFieldId> FieldIdFromType(const string &name, const LogicalType &type, idx_t &column_id) {
	auto field_index = FieldIndex(column_id++);
	vector<unique_ptr<DuckLakeFieldId>> field_children;
	switch(type.id()) {
	case LogicalTypeId::STRUCT: {
		for(auto &entry : StructType::GetChildTypes(type)) {
			field_children.push_back(FieldIdFromType(entry.first, entry.second, column_id));
		}
		break;
	}
	case LogicalTypeId::LIST:
		field_children.push_back(FieldIdFromType("element", ListType::GetChildType(type), column_id));
		break;
	case LogicalTypeId::ARRAY:
		field_children.push_back(FieldIdFromType("element", ArrayType::GetChildType(type), column_id));
		break;
	case LogicalTypeId::MAP:
		field_children.push_back(FieldIdFromType("key", MapType::KeyType(type), column_id));
		field_children.push_back(FieldIdFromType("value", MapType::ValueType(type), column_id));
		break;
	default:
		break;
	}
	return make_uniq<DuckLakeFieldId>(field_index, name, type, std::move(field_children));
}

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
    auto &base_info = info.Base();
	auto &duck_transaction = transaction.transaction->Cast<DuckLakeTransaction>();
	//! get a local table-id
	auto table_id = TableIndex(duck_transaction.GetLocalCatalogId());
	auto table_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	// generate field ids based on the column ids
	idx_t column_id = 0;
	auto field_data = make_shared_ptr<DuckLakeFieldData>();
	for(auto &col : base_info.columns.Logical()) {
		auto field_id = FieldIdFromType(col.Name(), col.Type(), column_id);
		field_data->Add(std::move(field_id));
	}
	auto table_entry = make_uniq<DuckLakeTableEntry>(ParentCatalog(), *this, base_info, table_id,
	                                                 std::move(table_uuid), std::move(field_data), TransactionLocalChange::CREATED);
	auto result = table_entry.get();
	duck_transaction.CreateEntry(std::move(table_entry));
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
		return tables;
	default:
		throw NotImplementedException("Unimplemented catalog type for schema");
	}
}

} // namespace duckdb
