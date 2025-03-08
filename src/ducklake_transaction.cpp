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
	if (ChangesMade()) {
		FlushChanges();
		connection.reset();
	} else if (connection) {
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

struct SnapshotChangeInformation {
	case_insensitive_set_t created_schemas;
	unordered_set<idx_t> dropped_schemas;
	case_insensitive_map_t<case_insensitive_set_t> created_tables;
	unordered_set<idx_t> dropped_tables;
	unordered_set<idx_t> inserted_tables;
};

SnapshotChangeInformation DuckLakeTransaction::GetSnapshotChanges() {
	SnapshotChangeInformation changes;
	for (auto &dropped_table_idx : dropped_tables) {
		changes.dropped_tables.insert(dropped_table_idx);
	}
	for (auto &dropped_schema_idx : dropped_schemas) {
		changes.dropped_schemas.insert(dropped_schema_idx);
	}
	if (new_schemas) {
		for (auto &entry : new_schemas->GetEntries()) {
			auto &schema_entry = entry.second->Cast<DuckLakeSchemaEntry>();
			changes.created_schemas.insert(schema_entry.name);
		}
	}
	for (auto &schema_entry : new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			// write any new tables that we created
			auto &table = entry.second->Cast<DuckLakeTableEntry>();
			auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
			changes.created_tables[schema.name].insert(table.name);
		}
	}
	for (auto &entry : new_data_files) {
		auto table_id = entry.first;
		changes.inserted_tables.insert(table_id);
	}
	return changes;
}

string SQLStringOrNull(const string &str) {
	if (str.empty()) {
		return "NULL";
	}
	return KeywordHelper::WriteQuoted(str, '\'');
}

void DuckLakeTransaction::WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot,
                                               const SnapshotChangeInformation &changes) {
	string schemas_created;
	string schemas_dropped;
	string tables_created;
	string tables_dropped;
	string tables_altered;
	string tables_inserted_into;
	string tables_deleted_from;

	for (auto &dropped_schema_idx : changes.dropped_schemas) {
		if (!schemas_dropped.empty()) {
			schemas_dropped += ",";
		}
		schemas_dropped += to_string(dropped_schema_idx);
	}
	for (auto &dropped_table_idx : changes.dropped_tables) {
		if (!tables_dropped.empty()) {
			tables_dropped += ",";
		}
		tables_dropped += to_string(dropped_table_idx);
	}
	for (auto &created_schema : changes.created_schemas) {
		if (!schemas_created.empty()) {
			schemas_created += ",";
		}
		schemas_created += KeywordHelper::WriteQuoted(created_schema, '"');
	}
	for (auto &entry : changes.created_tables) {
		auto &schema = entry.first;
		auto schema_prefix = KeywordHelper::WriteQuoted(schema, '"') + ".";
		for (auto &created_table : entry.second) {
			if (!tables_created.empty()) {
				tables_created += ",";
			}
			tables_created += schema_prefix + KeywordHelper::WriteQuoted(created_table, '"');
		}
	}
	// insert the snapshot changes
	auto query = StringUtil::Format(
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes VALUES ({SNAPSHOT_ID}, %s, %s, %s, %s, %s, %s, %s);)",
	    SQLStringOrNull(schemas_created), SQLStringOrNull(schemas_dropped), SQLStringOrNull(tables_created),
	    SQLStringOrNull(tables_dropped), SQLStringOrNull(tables_altered), SQLStringOrNull(tables_inserted_into),
	    SQLStringOrNull(tables_deleted_from));
	auto result = Query(commit_snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new snapshot to DuckLake:");
	}
}

string ParseQuotedValue(const string &input, idx_t &pos) {
	if (pos >= input.size() || input[pos] != '"') {
		throw InvalidInputException("Failed to parse quoted value - expected a quote");
	}
	string result;
	pos++;
	for (; pos < input.size(); pos++) {
		if (input[pos] == '"') {
			pos++;
			// check if this is an escaped quote
			if (pos < input.size() && input[pos] == '"') {
				// escaped quote
				result += '"';
				continue;
			}
			return result;
		}
		result += input[pos];
	}
	throw InvalidInputException("Failed to parse quoted value - unterminated quote");
}

vector<string> ParseSchemaList(const string &input) {
	vector<string> result;
	if (input.empty()) {
		return result;
	}
	idx_t pos = 0;
	while (true) {
		result.push_back(ParseQuotedValue(input, pos));
		if (pos >= input.size()) {
			break;
		}
		if (input[pos] != ',') {
			throw InvalidInputException("Failed to parse schema list - expected a comma");
		}
		pos++;
	}
	return result;
}

struct ParsedTableInfo {
	string schema;
	string table;
};

vector<ParsedTableInfo> ParseTableList(const string &input) {
	vector<ParsedTableInfo> result;
	if (input.empty()) {
		return result;
	}
	idx_t pos = 0;
	while (true) {
		ParsedTableInfo table_data;
		table_data.schema = ParseQuotedValue(input, pos);
		if (pos >= input.size() || input[pos] != '.') {
			throw InvalidInputException("Failed to parse table list - expected a dot");
		}
		pos++;
		table_data.table = ParseQuotedValue(input, pos);
		result.push_back(std::move(table_data));
		if (pos >= input.size()) {
			break;
		}
		if (input[pos] != ',') {
			throw InvalidInputException("Failed to parse table list - expected a comma");
		}
		pos++;
	}
	return result;
}

void DuckLakeTransaction::CheckForConflicts(const SnapshotChangeInformation &changes,
                                            const SnapshotChangeInformation &other_changes) {
	// check if we are creating the same table as another transaction
	for (auto &entry : changes.created_tables) {
		auto &schema_name = entry.first;
		auto other_entry = other_changes.created_tables.find(schema_name);
		if (other_entry == other_changes.created_tables.end()) {
			// the other transactions created no tables in this schema
			continue;
		}
		auto &created_tables = entry.second;
		auto &other_created_tables = other_entry->second;
		for (auto &table_name : created_tables) {
			if (other_created_tables.find(table_name) != other_created_tables.end()) {
				throw TransactionException("Transaction conflict - attempting to create table \"%s\" in schema \"%s\" "
				                           "- but this has been created by another transaction already",
				                           table_name, schema_name);
			}
		}
	}
}

void DuckLakeTransaction::CheckForConflicts(DuckLakeSnapshot transaction_snapshot,
                                            const SnapshotChangeInformation &changes) {
	// get all changes made to the system after the current snapshot was started
	auto result = Query(transaction_snapshot, R"(
	SELECT COALESCE(STRING_AGG(schemas_created), ''),
		   COALESCE(STRING_AGG(schemas_dropped), ''),
		   COALESCE(STRING_AGG(tables_created), ''),
		   COALESCE(STRING_AGG(tables_dropped), ''),
		   COALESCE(STRING_AGG(tables_altered), ''),
		   COALESCE(STRING_AGG(tables_inserted_into), ''),
		   COALESCE(STRING_AGG(tables_deleted_from), '')
	FROM {METADATA_CATALOG}.ducklake_snapshot_changes
	WHERE snapshot_id > {SNAPSHOT_ID}
	)");
	if (result->HasError()) {
		result->GetErrorObject().Throw(
		    "Failed to commit DuckLake transaction - failed to get snapshot changes for conflict resolution:");
	}
	// parse changes made by other transactions
	SnapshotChangeInformation other_changes;
	for (auto &row : *result) {
		auto created_schemas = row.GetValue<string>(0);
		auto dropped_schemas = row.GetValue<string>(1);
		auto created_tables = row.GetValue<string>(2);

		auto created_schema_list = ParseSchemaList(created_schemas);
		for (auto &created_schema : created_schema_list) {
			other_changes.created_schemas.insert(created_schema);
		}
		auto created_table_list = ParseTableList(created_tables);
		for (auto &entry : created_table_list) {
			other_changes.created_tables[entry.schema].insert(entry.table);
		}
	}
	CheckForConflicts(changes, other_changes);
}

void DuckLakeTransaction::FlushChanges() {
	if (!ChangesMade()) {
		// read-only transactions don't need to do anything
		return;
	}
	idx_t max_retry_count = 5;
	auto transaction_snapshot = GetSnapshot();
	auto transaction_changes = GetSnapshotChanges();
	for (idx_t i = 0; i < max_retry_count; i++) {
		auto commit_snapshot = GetSnapshot();
		commit_snapshot.snapshot_id++;
		if (SchemaChangesMade()) {
			// we changed the schema - need to get a new schema version
			commit_snapshot.schema_version++;
		}
		try {
			if (i > 0) {
				// we failed our first commit due to another transaction committing
				// retry - but first check for conflicts
				CheckForConflicts(transaction_snapshot, transaction_changes);
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
							column_insert_query += StringUtil::Format(
							    "(%d, {SNAPSHOT_ID}, NULL, %d, %d, %s, %s, NULL)", column_id, table_id, column_id,
							    SQLString(col.GetName()), SQLString(DuckLakeTypes::ToString(col.GetType())));
						}
						column_insert_query =
						    "INSERT INTO {METADATA_CATALOG}.ducklake_column VALUES " + column_insert_query;
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
				// 	CREATE TABLE ducklake_data_file(data_file_id BIGINT PRIMARY KEY, begin_snapshot BIGINT, end_snapshot
				// BIGINT, table_id BIGINT, file_order BIGINT, path VARCHAR, file_format VARCHAR, record_count BIGINT,
				// file_size_bytes BIGINT, partition_id BIGINT);
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
				data_file_insert_query =
				    "INSERT INTO {METADATA_CATALOG}.ducklake_data_file VALUES " + data_file_insert_query;
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
			WriteSnapshotChanges(commit_snapshot, transaction_changes);
			connection->Commit();
			// finished writing
			break;
		} catch (std::exception &ex) {
			ErrorData error(ex);
			connection->Rollback();
			if (error.Type() == ExceptionType::TRANSACTION) {
				// immediately rethrow transaction conflicts - no need to retry
				connection.reset();
				throw;
			}
			bool is_primary_key_error = StringUtil::Contains(error.Message(), "primary key constraint");
			bool finished_retrying = i + 1 >= max_retry_count;
			if (!is_primary_key_error || finished_retrying) {
				// we abort after the max retry count
				error.Throw("Failed to commit DuckLake transaction:");
			}
			// retry the transaction (with a new snapshot id)
			connection->BeginTransaction();
			snapshot.reset();
		}
	}
}

unique_ptr<QueryResult> DuckLakeTransaction::Query(string query) {
	auto &connection = GetConnection();
	// FIXME: escaping...
	query = StringUtil::Replace(query, "{METADATA_CATALOG_NAME}", ducklake_catalog.MetadataDatabaseName());
	// FIXME: configurable schema, and default to default schema for catalog
	query = StringUtil::Replace(query, "{METADATA_CATALOG}", ducklake_catalog.MetadataDatabaseName() + ".main");
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
