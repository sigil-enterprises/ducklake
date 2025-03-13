#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"

#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

DuckLakeTransaction::DuckLakeTransaction(DuckLakeCatalog &ducklake_catalog, TransactionManager &manager,
                                         ClientContext &context)
    : Transaction(manager, context), ducklake_catalog(ducklake_catalog), db(*context.db),
      local_catalog_id(TRANSACTION_LOCAL_ID_START) {
	metadata_manager = make_uniq<DuckLakeMetadataManager>(*this);
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
	CleanupFiles();
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

struct TransactionChangeInformation {
	case_insensitive_set_t created_schemas;
	unordered_map<idx_t, reference<DuckLakeSchemaEntry>> dropped_schemas;
	case_insensitive_map_t<reference_set_t<DuckLakeTableEntry>> created_tables;
	unordered_set<idx_t> dropped_tables;
	unordered_set<idx_t> inserted_tables;
};

struct SnapshotChangeInformation {
	case_insensitive_set_t created_schemas;
	unordered_set<idx_t> dropped_schemas;
	case_insensitive_map_t<case_insensitive_set_t> created_tables;
	unordered_set<idx_t> dropped_tables;
	unordered_set<idx_t> inserted_tables;
};

TransactionChangeInformation DuckLakeTransaction::GetTransactionChanges() {
	TransactionChangeInformation changes;
	for (auto &dropped_table_idx : dropped_tables) {
		changes.dropped_tables.insert(dropped_table_idx);
	}
	for (auto &entry : dropped_schemas) {
		changes.dropped_schemas.insert(entry);
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
			changes.created_tables[schema.name].insert(table);
		}
	}
	for (auto &entry : new_data_files) {
		auto table_id = entry.first;
		if (IsTransactionLocal(table_id)) {
			// don't report transaction-local tables yet - these will get added later on
			continue;
		}
		changes.inserted_tables.insert(table_id);
	}
	return changes;
}

void DuckLakeTransaction::WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot,
                                               TransactionChangeInformation &changes) {
	SnapshotChangeInfo change_info;

	// re-add all inserted tables - transaction-local table identifiers should have been converted at this stage
	for (auto &entry : new_data_files) {
		auto table_id = entry.first;
		changes.inserted_tables.insert(table_id);
	}
	for (auto &entry : changes.dropped_schemas) {
		if (!change_info.schemas_dropped.empty()) {
			change_info.schemas_dropped += ",";
		}
		change_info.schemas_dropped += to_string(entry.first);
	}
	for (auto &dropped_table_idx : changes.dropped_tables) {
		if (!change_info.tables_dropped.empty()) {
			change_info.tables_dropped += ",";
		}
		change_info.tables_dropped += to_string(dropped_table_idx);
	}
	for (auto &created_schema : changes.created_schemas) {
		if (!change_info.schemas_created.empty()) {
			change_info.schemas_created += ",";
		}
		change_info.schemas_created += KeywordHelper::WriteQuoted(created_schema, '"');
	}
	for (auto &entry : changes.created_tables) {
		auto &schema = entry.first;
		auto schema_prefix = KeywordHelper::WriteQuoted(schema, '"') + ".";
		for (auto &created_table : entry.second) {
			if (!change_info.tables_created.empty()) {
				change_info.tables_created += ",";
			}
			change_info.tables_created += schema_prefix + KeywordHelper::WriteQuoted(created_table.get().name, '"');
		}
	}
	for (auto &inserted_table_idx : changes.inserted_tables) {
		if (!change_info.tables_inserted_into.empty()) {
			change_info.tables_inserted_into += ",";
		}
		change_info.tables_inserted_into += to_string(inserted_table_idx);
	}
	metadata_manager->WriteSnapshotChanges(commit_snapshot, change_info);
}

void DuckLakeTransaction::CleanupFiles() {
	if (new_data_files.empty()) {
		return;
	}
	// remove any files that were written
	auto &fs = FileSystem::GetFileSystem(db);
	for (auto &table_entry : new_data_files) {
		for (auto &file : table_entry.second) {
			fs.RemoveFile(file.file_name);
		}
	}
	new_data_files.clear();
}

void DuckLakeTransaction::CheckForConflicts(const TransactionChangeInformation &changes,
                                            const SnapshotChangeInformation &other_changes) {
	// check if we are dropping the same table as another transaction
	for (auto &dropped_idx : changes.dropped_tables) {
		if (other_changes.dropped_tables.find(dropped_idx) != other_changes.dropped_tables.end()) {
			throw TransactionException("Transaction conflict - attempting to drop table with table index \"%s\""
			                           "- but this has been dropped by another transaction already",
			                           dropped_idx);
		}
	}
	// check if we are dropping the same schema as another transaction
	for (auto &entry : changes.dropped_schemas) {
		auto &dropped_schema = entry.second.get();
		auto dropped_idx = entry.first;
		if (other_changes.dropped_schemas.find(dropped_idx) != other_changes.dropped_schemas.end()) {
			throw TransactionException("Transaction conflict - attempting to drop schema \"%s\""
			                           "- but this has been dropped by another transaction already",
			                           dropped_schema.name);
		}
		auto other_entry = other_changes.created_tables.find(dropped_schema.name);
		if (other_entry != other_changes.created_tables.end()) {
			throw TransactionException("Transaction conflict - attempting to drop schema \"%s\""
			                           "- but another transaction has created a table in this schema",
			                           dropped_schema.name);
		}
	}
	// check if we are creating the same schema as another transaction
	for (auto &created_schema : changes.created_schemas) {
		if (other_changes.created_schemas.find(created_schema) != other_changes.created_schemas.end()) {
			throw TransactionException("Transaction conflict - attempting to create schema \"%s\""
			                           "- but this has been created by another transaction already",
			                           created_schema);
		}
	}
	// check if we are creating the same table as another transaction
	for (auto &entry : changes.created_tables) {
		auto &schema_name = entry.first;
		auto &created_tables = entry.second;
		for (auto &table_ref : created_tables) {
			auto &table = table_ref.get();
			auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
			auto schema_entry = other_changes.dropped_schemas.find(schema.GetSchemaId());
			if (schema_entry != other_changes.dropped_schemas.end()) {
				// the schema this table was created in was dropped
				throw TransactionException("Transaction conflict - attempting to create table \"%s\" in schema \"%s\" "
				                           "- but this schema has been dropped by another transaction already",
				                           table.name, schema_name);
			}
			auto other_entry = other_changes.created_tables.find(schema_name);
			if (other_entry == other_changes.created_tables.end()) {
				// the other transactions created no tables in this schema
				continue;
			}
			auto &other_created_tables = other_entry->second;
			if (other_created_tables.find(table.name) != other_created_tables.end()) {
				// a table with this name in this schema was already created
				throw TransactionException("Transaction conflict - attempting to create table \"%s\" in schema \"%s\" "
				                           "- but this table has been created by another transaction already",
				                           table.name, schema_name);
			}
		}
	}
	for (auto &table_id : changes.inserted_tables) {
		auto other_entry = other_changes.dropped_tables.find(table_id);
		if (other_entry != other_changes.dropped_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to insert into table with id %d"
			                           "- but this table has been dropped by another transaction",
			                           table_id);
		}
	}
}

void DuckLakeTransaction::CheckForConflicts(DuckLakeSnapshot transaction_snapshot,
                                            const TransactionChangeInformation &changes) {

	// get all changes made to the system after the current snapshot was started
	auto changes_made = metadata_manager->GetChangesMadeAfterSnapshot(transaction_snapshot);
	// parse changes made by other transactions
	SnapshotChangeInformation other_changes;
	auto created_schema_list = DuckLakeUtil::ParseQuotedList(changes_made.schemas_created);
	for (auto &created_schema : created_schema_list) {
		other_changes.created_schemas.insert(created_schema);
	}
	auto created_table_list = DuckLakeUtil::ParseTableList(changes_made.tables_created);
	for (auto &entry : created_table_list) {
		other_changes.created_tables[entry.schema].insert(entry.table);
	}
	other_changes.dropped_schemas = DuckLakeUtil::ParseDropList(changes_made.schemas_dropped);
	other_changes.dropped_tables = DuckLakeUtil::ParseDropList(changes_made.tables_dropped);
	CheckForConflicts(changes, other_changes);
}

vector<DuckLakeSchemaInfo> DuckLakeTransaction::GetNewSchemas(DuckLakeSnapshot &commit_snapshot) {
	vector<DuckLakeSchemaInfo> schemas;
	for (auto &entry : new_schemas->GetEntries()) {
		auto &schema_entry = entry.second->Cast<DuckLakeSchemaEntry>();
		DuckLakeSchemaInfo schema_info;
		schema_info.id = commit_snapshot.next_catalog_id++;
		schema_info.uuid = schema_entry.GetSchemaUUID();
		schema_info.name = schema_entry.name;

		// set the schema id of this schema entry so subsequent tables are written correctly
		schema_entry.SetSchemaId(schema_info.id);

		// add the schema to the list
		schemas.push_back(std::move(schema_info));
	}
	return schemas;
}

void DuckLakeTransaction::GetNewPartitionKey(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table, vector<DuckLakePartitionInfo> &new_partition_keys) {
	DuckLakePartitionInfo partition_key;
	partition_key.table_id = table.GetTableId();
	// insert the new partition data
	auto partition_data = table.GetPartitionData();
	if (!partition_data) {
		// dropping partition data - insert the empty partition key data for this table
		new_partition_keys.push_back(std::move(partition_key));
		return;
	}
	auto partition_id = commit_snapshot.next_catalog_id++;
	partition_key.id = partition_id;
	partition_data->partition_id = partition_id;
	for (auto &field : partition_data->fields) {
		partition_key.fields.push_back(field);
	}
	new_partition_keys.push_back(std::move(partition_key));
}

vector<DuckLakeTableInfo> DuckLakeTransaction::GetNewTables(DuckLakeSnapshot &commit_snapshot, vector<DuckLakePartitionInfo> &new_partition_keys) {
	vector<DuckLakeTableInfo> tables;
	for (auto &schema_entry : new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			// write any new tables that we created
			auto &table = entry.second->Cast<DuckLakeTableEntry>();
			if (table.LocalChange() == TransactionLocalChange::SET_PARTITION_KEY) {
				GetNewPartitionKey(commit_snapshot, table, new_partition_keys);
				continue;
			}
			auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
			DuckLakeTableInfo table_entry;
			auto original_id = table.GetTableId();
			bool is_new_table;
			if (IsTransactionLocal(original_id)) {
				table_entry.id = commit_snapshot.next_catalog_id++;
				is_new_table = true;
			} else {
				// this table already has an id - keep it
				// this happens if e.g. this table is renamed
				table_entry.id = original_id;
				is_new_table = false;
			}
			table_entry.uuid = table.GetTableUUID();
			table_entry.schema_id = schema.GetSchemaId();
			table_entry.name = table.name;
			if (is_new_table) {
				// if this is a new table - write the columns
				idx_t column_id = 0;

				for (auto &col : table.GetColumns().Logical()) {
					DuckLakeColumnInfo column_entry;
					column_entry.id = column_id;
					column_entry.name = col.GetName();
					column_entry.type = DuckLakeTypes::ToString(col.GetType());
					table_entry.columns.push_back(std::move(column_entry));
					column_id++;
				}
				// if we have written any data to this table - move them to the new (correct) table id as well
				auto data_file_entry = new_data_files.find(original_id);
				if (data_file_entry != new_data_files.end()) {
					new_data_files[table_entry.id] = std::move(data_file_entry->second);
					new_data_files.erase(original_id);
				}
			}
			tables.push_back(std::move(table_entry));
		}
	}
	return tables;
}

void DuckLakeTransaction::UpdateGlobalTableStats(idx_t table_id, DuckLakeTableStats new_stats) {
	auto current_stats = ducklake_catalog.GetTableStats(*this, table_id);
	bool stats_initialized = false;
	if (current_stats) {
		// merge the current stats into the new stats
		for (auto &entry : current_stats->column_stats) {
			new_stats.MergeStats(entry.first, entry.second);
		}
		new_stats.record_count += current_stats->record_count;
		new_stats.table_size_bytes += current_stats->table_size_bytes;
		stats_initialized = true;
	}

	// now that we have obtained the total stats - generate the SQL to insert
	string column_stats_values;
	for (auto &entry : new_stats.column_stats) {
		if (!column_stats_values.empty()) {
			column_stats_values += ",";
		}
		auto column_id = entry.first;
		auto &column_stats = entry.second;
		column_stats_values += "(";
		column_stats_values += to_string(table_id);
		column_stats_values += ",";
		column_stats_values += to_string(column_id);
		column_stats_values += ",";
		if (column_stats.has_null_count) {
			column_stats_values += column_stats.null_count > 0 ? "true" : "false";
		} else {
			column_stats_values += "NULL";
		}
		column_stats_values += ",";
		if (column_stats.has_min) {
			column_stats_values += DuckLakeUtil::SQLLiteralToString(column_stats.min);
		} else {
			column_stats_values += "NULL";
		}
		column_stats_values += ",";
		if (entry.second.has_max) {
			column_stats_values += DuckLakeUtil::SQLLiteralToString(column_stats.max);
		} else {
			column_stats_values += "NULL";
		}
		column_stats_values += ")";
	}
	// finally update the stats in the tables
	if (!stats_initialized) {
		// stats have not been initialized yet - insert them
		auto result =
		    Query(StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_table_stats VALUES (%d, %d, %d);",
		                             table_id, new_stats.record_count, new_stats.table_size_bytes));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to insert stats information in DuckLake: ");
		}

		result = Query(StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_table_column_stats VALUES %s;",
		                                  column_stats_values));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to insert stats information in DuckLake: ");
		}
	} else {
		// stats have been initialized - update them
		auto result = Query(StringUtil::Format(
		    "UPDATE {METADATA_CATALOG}.ducklake_table_stats SET record_count=%d, file_size_bytes=%d WHERE table_id=%d;",
		    new_stats.record_count, new_stats.table_size_bytes, table_id));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to update stats information in DuckLake: ");
		}
		result = Query(StringUtil::Format(R"(
WITH new_values(tid, cid, new_contains_null, new_min, new_max) AS (
	VALUES %s
)
UPDATE {METADATA_CATALOG}.ducklake_table_column_stats
SET contains_null=new_contains_null, min_value=new_min, max_value=new_max
FROM new_values
WHERE table_id=tid AND column_id=cid
)",
		                                  column_stats_values));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to update stats information in DuckLake: ");
		}
	}
}

vector<DuckLakeFileInfo> DuckLakeTransaction::GetNewDataFiles(DuckLakeSnapshot &commit_snapshot) {
	vector<DuckLakeFileInfo> result;
	for (auto &entry : new_data_files) {
		auto table_id = entry.first;
		if (IsTransactionLocal(table_id)) {
			throw InternalException("Cannot commit transaction local files - these should have been cleaned up before");
		}

		DuckLakeTableStats new_stats;
		for (auto &file : entry.second) {
			DuckLakeFileInfo data_file;
			data_file.id = commit_snapshot.next_file_id++;
			data_file.table_id = table_id;
			data_file.file_name = file.file_name;
			data_file.row_count = file.row_count;
			data_file.file_size_bytes = file.file_size_bytes;
			data_file.footer_size = file.footer_size;

			new_stats.record_count += file.row_count;
			new_stats.table_size_bytes += file.file_size_bytes;

			// gather the column statistics for this file
			for (auto &column_stats_entry : file.column_stats) {
				DuckLakeColumnStatsInfo column_stats;
				column_stats.column_id = column_stats_entry.first;
				auto &stats = column_stats_entry.second;
				column_stats.min_val = stats.has_min ? DuckLakeUtil::SQLLiteralToString(stats.min) : "NULL";
				column_stats.max_val = stats.has_max ? DuckLakeUtil::SQLLiteralToString(stats.max) : "NULL";
				if (stats.has_null_count) {
					column_stats.value_count = to_string(file.row_count - stats.null_count);
					column_stats.null_count = to_string(stats.null_count);
					if (stats.null_count == file.row_count) {
						// all values are NULL for this file
						stats.any_valid = false;
					}
				} else {
					column_stats.value_count = "NULL";
					column_stats.null_count = "NULL";
				}

				// merge the stats into the new global states
				new_stats.MergeStats(column_stats.column_id, stats);

				data_file.column_stats.push_back(std::move(column_stats));
			}
			result.push_back(std::move(data_file));
		}
		// update the global stats for this table based on the newly written files
		UpdateGlobalTableStats(table_id, std::move(new_stats));
	}
	return result;
}

void DuckLakeTransaction::FlushChanges() {
	if (!ChangesMade()) {
		// read-only transactions don't need to do anything
		return;
	}
	idx_t max_retry_count = 5;
	auto transaction_snapshot = GetSnapshot();
	auto transaction_changes = GetTransactionChanges();
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
			// drop entries
			if (!dropped_tables.empty()) {
				metadata_manager->DropTables(commit_snapshot, dropped_tables);
			}
			if (!dropped_schemas.empty()) {
				unordered_set<idx_t> dropped_schema_ids;
				for (auto &entry : dropped_schemas) {
					dropped_schema_ids.insert(entry.first);
				}
				metadata_manager->DropSchemas(commit_snapshot, dropped_schema_ids);
			}
			// write new schemas
			if (new_schemas) {
				auto schema_list = GetNewSchemas(commit_snapshot);
				metadata_manager->WriteNewSchemas(commit_snapshot, schema_list);
			}

			// write new tables
			if (!new_tables.empty()) {
				vector<DuckLakePartitionInfo> partition_keys;
				auto table_list = GetNewTables(commit_snapshot, partition_keys);
				metadata_manager->WriteNewTables(commit_snapshot, table_list);
				metadata_manager->WriteNewPartitionKeys(commit_snapshot, partition_keys);
			}

			// write new data files
			if (!new_data_files.empty()) {
				auto file_list = GetNewDataFiles(commit_snapshot);
				metadata_manager->WriteNewDataFiles(commit_snapshot, file_list);
			}

			// write the new snapshot
			metadata_manager->InsertSnapshot(commit_snapshot);

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
				CleanupFiles();
				throw;
			}
			bool is_primary_key_error = StringUtil::Contains(error.Message(), "primary key constraint");
			bool finished_retrying = i + 1 >= max_retry_count;
			if (!is_primary_key_error || finished_retrying) {
				// we abort after the max retry count
				CleanupFiles();
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
	auto catalog_identifier = DuckLakeUtil::SQLIdentifierToString(ducklake_catalog.MetadataDatabaseName());
	auto catalog_literal = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataDatabaseName());
	auto schema_identifier = DuckLakeUtil::SQLIdentifierToString(ducklake_catalog.MetadataSchemaName());
	auto schema_literal = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataSchemaName());
	auto metadata_path = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataPath());
	auto data_path = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.DataPath());

	query = StringUtil::Replace(query, "{METADATA_CATALOG_NAME_LITERAL}", catalog_literal);
	query = StringUtil::Replace(query, "{METADATA_CATALOG_NAME_IDENTIFIER}", catalog_identifier);
	query = StringUtil::Replace(query, "{METADATA_SCHEMA_NAME_LITERAL}", schema_literal);
	query = StringUtil::Replace(query, "{METADATA_CATALOG}", catalog_identifier + "." + schema_identifier);
	query = StringUtil::Replace(query, "{METADATA_PATH}", metadata_path);
	query = StringUtil::Replace(query, "{DATA_PATH}", data_path);
	return connection.Query(query);
}

unique_ptr<QueryResult> DuckLakeTransaction::Query(DuckLakeSnapshot snapshot, string query) {
	query = StringUtil::Replace(query, "{SNAPSHOT_ID}", to_string(snapshot.snapshot_id));
	query = StringUtil::Replace(query, "{SCHEMA_VERSION}", to_string(snapshot.schema_version));
	query = StringUtil::Replace(query, "{NEXT_CATALOG_ID}", to_string(snapshot.next_catalog_id));
	query = StringUtil::Replace(query, "{NEXT_FILE_ID}", to_string(snapshot.next_file_id));
	return Query(std::move(query));
}

string DuckLakeTransaction::GetDefaultSchemaName() {
	auto &metadata_context = *connection->context;
	auto &db_manager = DatabaseManager::Get(metadata_context);
	auto metadb = db_manager.GetDatabase(metadata_context, ducklake_catalog.MetadataDatabaseName());
	return metadb->GetCatalog().GetDefaultSchema();
}

DuckLakeSnapshot DuckLakeTransaction::GetSnapshot() {
	if (!snapshot) {
		// no snapshot loaded yet for this transaction - load it
		snapshot = metadata_manager->GetSnapshot();
	}
	return *snapshot;
}

idx_t DuckLakeTransaction::GetLocalCatalogId() {
	return local_catalog_id++;
}

vector<DuckLakeDataFile> DuckLakeTransaction::GetTransactionLocalFiles(idx_t table_id) {
	auto entry = new_data_files.find(table_id);
	if (entry == new_data_files.end()) {
		return vector<DuckLakeDataFile>();
	} else {
		return entry->second;
	}
}

void DuckLakeTransaction::AppendFiles(idx_t table_id, const vector<DuckLakeDataFile> &files) {
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
		if (new_schemas->GetEntries().size() == 0) {
			// we have dropped all schemas created in this transaction - clear it
			new_schemas.reset();
		}
	} else {
		dropped_schemas.insert(make_pair(schema.GetSchemaId(), reference<DuckLakeSchemaEntry>(schema)));
	}
}

void DuckLakeTransaction::DropTable(DuckLakeTableEntry &table) {
	if (table.IsTransactionLocal()) {
		// table is transaction-local - drop it from the transaction local changes
		auto schema_entry = new_tables.find(table.ParentSchema().name);
		if (schema_entry == new_tables.end()) {
			throw InternalException("Dropping a transaction local table that does not exist?");
		}
		auto table_id = table.GetTableId();
		schema_entry->second->DropEntry(table.name);
		// if we have written any files for this table - clean them up
		auto table_entry = new_data_files.find(table_id);
		if (table_entry != new_data_files.end()) {
			auto &fs = FileSystem::GetFileSystem(db);
			for (auto &file : table_entry->second) {
				fs.RemoveFile(file.file_name);
			}
			new_data_files.erase(table_entry);
		}
		new_tables.erase(schema_entry);
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
	auto &new_table = new_entry->Cast<DuckLakeTableEntry>();
	auto &entries = GetOrCreateTransactionLocalEntries(entry);
	entries.CreateEntry(std::move(new_entry));
	switch (new_table.LocalChange()) {
	case TransactionLocalChange::RENAMED: {
		// rename - take care of the old table
		if (table.IsTransactionLocal()) {
			// table is transaction local - delete the old table from there
			entries.DropEntry(entry.name);
		} else {
			// table is not transaction local - add to drop list
			auto table_id = table.GetTableId();
			dropped_tables.insert(table_id);
		}
		break;
	}
	case TransactionLocalChange::SET_PARTITION_KEY:
		break;
	default:
		throw NotImplementedException("Alter type not supported in DuckLakeTransaction::AlterEntry");
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
