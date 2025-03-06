#include "ducklake_transaction.hpp"
#include "duckdb/main/database_manager.hpp"
#include "ducklake_catalog.hpp"
#include "ducklake_schema_entry.hpp"
#include "ducklake_table_entry.hpp"
#include "ducklake_types.hpp"

namespace duckdb {

DuckLakeTransaction::DuckLakeTransaction(DuckLakeCatalog &ducklake_catalog, TransactionManager &manager,
                                         ClientContext &context)
    : Transaction(manager, context), ducklake_catalog(ducklake_catalog), db(*context.db),
      local_catalog_id(TRANSACTION_LOCAL_ID_START) {
}

DuckLakeTransaction::~DuckLakeTransaction() {
}

void DuckLakeTransaction::Start() {
}

void DuckLakeTransaction::Commit() {
	if (connection || ChangesMade()) {
		FlushChanges();
		connection->Commit();
		connection.reset();
	}
}

void DuckLakeTransaction::Rollback() {
	if (connection) {
		// rollback any changes made to the metadata catalog
		connection->Rollback();
		connection.reset();
	}
	if (!new_data_files.empty()) {
		// remove any files that were written
		auto &fs = FileSystem::GetFileSystem(db);
		for (auto &table_entry : new_data_files) {
			for (auto &file_name : table_entry.second) {
				fs.RemoveFile(file_name);
			}
		}
	}
}

Connection &DuckLakeTransaction::GetConnection() {
	if (!connection) {
		connection = make_uniq<Connection>(db);
		connection->BeginTransaction();
	}
	return *connection;
}

bool DuckLakeTransaction::SchemaChangesMade() {
	return !new_tables.empty() || !dropped_tables.empty() || new_schemas || !dropped_schemas.empty();
}

bool DuckLakeTransaction::ChangesMade() {
	return SchemaChangesMade() || !new_data_files.empty();
}

void DuckLakeTransaction::FlushDrop(DuckLakeSnapshot commit_snapshot, const string &metadata_table_name,
                                    const string &id_name, unordered_set<idx_t> &dropped_entries) {
	string dropped_table_list;
	for (auto &dropped_table_id : dropped_entries) {
		if (!dropped_table_list.empty()) {
			dropped_table_list += ", ";
		}
		dropped_table_list += to_string(dropped_table_id);
	}
	auto drop_tables_query =
	    StringUtil::Format(R"(UPDATE {METADATA_CATALOG}.%s SET end_snapshot = {SNAPSHOT_ID} WHERE %s IN (%s);)",
	                       metadata_table_name, id_name, dropped_table_list);
	auto result = Query(commit_snapshot, drop_tables_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write dropped table information to DuckLake:");
	}
}

void DuckLakeTransaction::FlushChanges() {
	if (!ChangesMade()) {
		// read-only transactions don't need to do anything
		return;
	}
	auto commit_snapshot = GetSnapshot();

	commit_snapshot.snapshot_id++;
	if (SchemaChangesMade()) {
		// we changed the schema - need to get a new schema version
		commit_snapshot.schema_version++;
	}
	unique_ptr<QueryResult> result;
	// drop entries
	if (!dropped_tables.empty()) {
		FlushDrop(commit_snapshot, "ducklake_table", "table_id", dropped_tables);
	}
	if (!dropped_schemas.empty()) {
		FlushDrop(commit_snapshot, "ducklake_schema", "schema_id", dropped_schemas);
	}
	// write new schemas
	if (new_schemas) {
		for (auto &entry : new_schemas->GetEntries()) {
			auto &schema_entry = entry.second->Cast<DuckLakeSchemaEntry>();
			auto schema_id = commit_snapshot.next_catalog_id++;
			auto schema_insert_query = StringUtil::Format(
			    R"(INSERT INTO {METADATA_CATALOG}.ducklake_schema VALUES (%d, '%s', {SNAPSHOT_ID}, NULL, %s);)",
			    schema_id, schema_entry.GetSchemaUUID(), SQLString(schema_entry.name));
			result = Query(commit_snapshot, schema_insert_query);
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to write new schema to DuckLake:");
			}

			// set the schema id of this schema entry so subsequent tables are written correctly
			schema_entry.SetSchemaId(schema_id);
		}
	}

	// write new tables
	for (auto &schema_entry : new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			// write any new tables that we created
			auto &table = entry.second->Cast<DuckLakeTableEntry>();
			auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
			auto original_id = table.GetTableId();
			idx_t table_id;
			bool is_new_table;
			if (IsTransactionLocal(original_id)) {
				table_id = commit_snapshot.next_catalog_id++;
				is_new_table = true;
			} else {
				// this table already has an id - keep it
				// this happens if e.g. this table is renamed
				table_id = original_id;
				is_new_table = false;
			}
			auto table_insert_query = StringUtil::Format(
			    R"(INSERT INTO {METADATA_CATALOG}.ducklake_table VALUES (%d, '%s', {SNAPSHOT_ID}, NULL, %d, %s);)",
			    table_id, table.GetTableUUID(), schema.GetSchemaId(), SQLString(table.name));
			result = Query(commit_snapshot, table_insert_query);
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to write new table to DuckLake:");
			}
			if (is_new_table) {
				// if this is a new table - write the columns
				string column_insert_query;
				idx_t column_id = 0;
				for (auto &col : table.GetColumns().Logical()) {
					if (!column_insert_query.empty()) {
						column_insert_query += ", ";
					}
					column_insert_query += StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, %d, %s, %s, NULL)",
					                                          column_id, table_id, column_id, SQLString(col.GetName()),
					                                          SQLString(DuckLakeTypes::ToString(col.GetType())));
				}
				column_insert_query = "INSERT INTO {METADATA_CATALOG}.ducklake_column VALUES " + column_insert_query;
				result = Query(commit_snapshot, column_insert_query);
				if (result->HasError()) {
					result->GetErrorObject().Throw("Failed to write column information to DuckLake:");
				}

				// if we have written any data to this table - move them to the new (correct) table id as well
				auto data_file_entry = new_data_files.find(original_id);
				if (data_file_entry != new_data_files.end()) {
					new_data_files[table_id] = std::move(data_file_entry->second);
					new_data_files.erase(original_id);
				}
			}
		}
	}
	// write new data files
	if (!new_data_files.empty()) {
		string data_file_insert_query;
		// 	CREATE TABLE ducklake_data_file(data_file_id BIGINT PRIMARY KEY, begin_snapshot BIGINT, end_snapshot BIGINT,
		// table_id BIGINT, file_order BIGINT, path VARCHAR, file_format VARCHAR, record_count BIGINT, file_size_bytes
		// BIGINT, partition_id BIGINT);
		for (auto &entry : new_data_files) {
			auto table_id = entry.first;
			for (auto &file : entry.second) {
				if (!data_file_insert_query.empty()) {
					data_file_insert_query += ",";
				}
				// FIXME: write file statistics
				data_file_insert_query +=
				    StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, NULL, %s, 'parquet', NULL, NULL, NULL)",
				                       commit_snapshot.next_file_id, table_id, SQLString(file));
				commit_snapshot.next_file_id++;
			}
		}
		if (data_file_insert_query.empty()) {
			throw InternalException("No files found!?");
		}
		data_file_insert_query = "INSERT INTO {METADATA_CATALOG}.ducklake_data_file VALUES " + data_file_insert_query;
		result = Query(commit_snapshot, data_file_insert_query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write data file information to DuckLake:");
		}
		// FIXME: write column statistics
	}
	// write the new snapshot
	result = Query(
	    commit_snapshot,
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES ({SNAPSHOT_ID}, NOW(), {SCHEMA_VERSION}, {NEXT_CATALOG_ID}, {NEXT_FILE_ID});)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new snapshot to DuckLake:");
	}
}

unique_ptr<QueryResult> DuckLakeTransaction::Query(string query) {
	auto &connection = GetConnection();
	query = StringUtil::Replace(query, "{METADATA_CATALOG}", ducklake_catalog.MetadataDatabaseName());
	query =
	    StringUtil::Replace(query, "{METADATA_PATH}", StringUtil::Replace(ducklake_catalog.MetadataPath(), "'", "''"));
	query = StringUtil::Replace(query, "{DATA_PATH}", StringUtil::Replace(ducklake_catalog.DataPath(), "'", "''"));
	return connection.Query(query);
}

unique_ptr<QueryResult> DuckLakeTransaction::Query(DuckLakeSnapshot snapshot, string query) {
	query = StringUtil::Replace(query, "{SNAPSHOT_ID}", to_string(snapshot.snapshot_id));
	query = StringUtil::Replace(query, "{SCHEMA_VERSION}", to_string(snapshot.schema_version));
	query = StringUtil::Replace(query, "{NEXT_CATALOG_ID}", to_string(snapshot.next_catalog_id));
	query = StringUtil::Replace(query, "{NEXT_FILE_ID}", to_string(snapshot.next_file_id));
	return Query(std::move(query));
}

DuckLakeSnapshot DuckLakeTransaction::GetSnapshot() {
	if (!snapshot) {
		// no snapshot loaded yet for this transaction
		// query the snapshot id/schema version
		auto result = Query(
		    R"(SELECT snapshot_id, schema_version, next_catalog_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id = (SELECT MAX(snapshot_id) FROM {METADATA_CATALOG}.ducklake_snapshot);)");
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to query most recent snapshot for DuckLake: ");
		}
		auto chunk = result->Fetch();
		if (chunk->size() != 1) {
			throw InvalidInputException("Corrupt DuckLake - multiple snapshots returned from database");
		}

		auto snapshot_id = chunk->GetValue(0, 0).GetValue<idx_t>();
		auto schema_version = chunk->GetValue(1, 0).GetValue<idx_t>();
		auto next_catalog_id = chunk->GetValue(2, 0).GetValue<idx_t>();
		auto next_file_id = chunk->GetValue(3, 0).GetValue<idx_t>();
		snapshot = make_uniq<DuckLakeSnapshot>(snapshot_id, schema_version, next_catalog_id, next_file_id);
	}
	return *snapshot;
}

idx_t DuckLakeTransaction::GetLocalCatalogId() {
	return local_catalog_id++;
}

vector<string> DuckLakeTransaction::GetTransactionLocalFiles(idx_t table_id) {
	auto entry = new_data_files.find(table_id);
	if (entry == new_data_files.end()) {
		return vector<string>();
	} else {
		return entry->second;
	}
}

void DuckLakeTransaction::AppendFiles(idx_t table_id, const vector<string> &files) {
	auto entry = new_data_files.find(table_id);
	if (entry != new_data_files.end()) {
		// already exists - append
		auto &existing_files = entry->second;
		existing_files.insert(existing_files.end(), files.begin(), files.end());
	} else {
		new_data_files.insert(make_pair(table_id, files));
	}
}

DuckLakeTransaction &DuckLakeTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<DuckLakeTransaction>();
}

void DuckLakeTransaction::CreateEntry(unique_ptr<CatalogEntry> entry) {
	auto &set = GetOrCreateTransactionLocalEntries(*entry);
	set.CreateEntry(std::move(entry));
}

void DuckLakeTransaction::DropSchema(DuckLakeSchemaEntry &schema) {
	auto schema_id = schema.GetSchemaId();
	if (IsTransactionLocal(schema_id)) {
		// schema is transaction-local - drop it from the transaction local changes
		if (!new_schemas) {
			throw InternalException("Dropping a transaction local table that does not exist?");
		}
		new_schemas->DropEntry(schema.name);
	} else {
		dropped_schemas.insert(schema_id);
	}
}

void DuckLakeTransaction::DropTable(DuckLakeTableEntry &table) {
	if (table.IsTransactionLocal()) {
		// table is transaction-local - drop it from the transaction local changes
		auto schema_entry = new_tables.find(table.ParentSchema().name);
		if (schema_entry == new_tables.end()) {
			throw InternalException("Dropping a transaction local table that does not exist?");
		}
		schema_entry->second->DropEntry(table.name);
	} else {
		auto table_id = table.GetTableId();
		dropped_tables.insert(table_id);
	}
}

void DuckLakeTransaction::DropEntry(CatalogEntry &entry) {
	switch (entry.type) {
	case CatalogType::TABLE_ENTRY:
		DropTable(entry.Cast<DuckLakeTableEntry>());
		break;
	case CatalogType::SCHEMA_ENTRY:
		DropSchema(entry.Cast<DuckLakeSchemaEntry>());
		break;
	default:
		throw InternalException("Unsupported type for drop");
	}
}

bool DuckLakeTransaction::IsDeleted(CatalogEntry &entry) {
	switch (entry.type) {
	case CatalogType::TABLE_ENTRY: {
		auto &table_entry = entry.Cast<DuckLakeTableEntry>();
		return dropped_tables.find(table_entry.GetTableId()) != dropped_tables.end();
	}
	case CatalogType::SCHEMA_ENTRY: {
		auto &schema_entry = entry.Cast<DuckLakeSchemaEntry>();
		return dropped_schemas.find(schema_entry.GetSchemaId()) != dropped_schemas.end();
	}
	default:
		throw InternalException("Catalog type not supported for IsDeleted");
	}
}

void DuckLakeTransaction::AlterEntry(CatalogEntry &entry, unique_ptr<CatalogEntry> new_entry) {
	if (entry.type != CatalogType::TABLE_ENTRY) {
		throw InternalException("Rename of this type not yet supported");
	}
	auto &table = entry.Cast<DuckLakeTableEntry>();
	auto &entries = GetOrCreateTransactionLocalEntries(entry);
	entries.CreateEntry(std::move(new_entry));
	if (table.IsTransactionLocal()) {
		// table is transaction local - delete the old table from there
		entries.DropEntry(entry.name);
	} else {
		// table is not transaction local - add to drop list
		auto table_id = table.GetTableId();
		dropped_tables.insert(table_id);
	}
}

DuckLakeCatalogSet &DuckLakeTransaction::GetOrCreateTransactionLocalEntries(CatalogEntry &entry) {
	auto catalog_type = entry.type;
	if (catalog_type == CatalogType::SCHEMA_ENTRY) {
		if (!new_schemas) {
			new_schemas = make_uniq<DuckLakeCatalogSet>();
		}
		return *new_schemas;
	}
	auto &schema_name = entry.ParentSchema().name;
	auto local_entry = GetTransactionLocalEntries(catalog_type, schema_name);
	if (local_entry) {
		return *local_entry;
	}
	if (catalog_type != CatalogType::TABLE_ENTRY) {
		throw InternalException("Catalog type not supported for transaction local storage");
	}
	// need to create it
	auto new_table_list = make_uniq<DuckLakeCatalogSet>();
	auto &result = *new_table_list;
	new_tables.insert(make_pair(schema_name, std::move(new_table_list)));
	return result;
}

optional_ptr<DuckLakeCatalogSet> DuckLakeTransaction::GetTransactionLocalSchemas() {
	return new_schemas;
}

optional_ptr<CatalogEntry> DuckLakeTransaction::GetTransactionLocalEntry(CatalogType catalog_type,
                                                                         const string &schema_name,
                                                                         const string &entry_name) {
	auto set = GetTransactionLocalEntries(catalog_type, schema_name);
	if (!set) {
		return nullptr;
	}
	return set->GetEntry(entry_name);
}

optional_ptr<DuckLakeCatalogSet> DuckLakeTransaction::GetTransactionLocalEntries(CatalogType catalog_type,
                                                                                 const string &schema_name) {
	if (catalog_type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto entry = new_tables.find(schema_name);
	if (entry == new_tables.end()) {
		return nullptr;
	}
	return entry->second;
}

} // namespace duckdb
