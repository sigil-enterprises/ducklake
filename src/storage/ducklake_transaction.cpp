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

string SQLStringOrNull(const string &str) {
	if (str.empty()) {
		return "NULL";
	}
	return KeywordHelper::WriteQuoted(str, '\'');
}

void DuckLakeTransaction::WriteSnapshotChanges(DuckLakeSnapshot commit_snapshot,
                                               TransactionChangeInformation &changes) {
	string schemas_created;
	string schemas_dropped;
	string tables_created;
	string tables_dropped;
	string tables_altered;
	string tables_inserted_into;
	string tables_deleted_from;

	// re-add all inserted tables - transaction-local table identifiers should have been converted at this stage
	for (auto &entry : new_data_files) {
		auto table_id = entry.first;
		changes.inserted_tables.insert(table_id);
	}
	for (auto &entry : changes.dropped_schemas) {
		if (!schemas_dropped.empty()) {
			schemas_dropped += ",";
		}
		schemas_dropped += to_string(entry.first);
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
			tables_created += schema_prefix + KeywordHelper::WriteQuoted(created_table.get().name, '"');
		}
	}
	for (auto &inserted_table_idx : changes.inserted_tables) {
		if (!tables_inserted_into.empty()) {
			tables_inserted_into += ",";
		}
		tables_inserted_into += to_string(inserted_table_idx);
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
		auto dropped_tables = row.GetValue<string>(3);

		auto created_schema_list = DuckLakeUtil::ParseQuotedList(created_schemas);
		for (auto &created_schema : created_schema_list) {
			other_changes.created_schemas.insert(created_schema);
		}
		auto created_table_list = DuckLakeUtil::ParseTableList(created_tables);
		for (auto &entry : created_table_list) {
			other_changes.created_tables[entry.schema].insert(entry.table);
		}
		other_changes.dropped_schemas = DuckLakeUtil::ParseDropList(dropped_schemas);
		other_changes.dropped_tables = DuckLakeUtil::ParseDropList(dropped_tables);
	}
	CheckForConflicts(changes, other_changes);
}

void DuckLakeTransaction::FlushNewSchemas(DuckLakeSnapshot &commit_snapshot) {
	if (!new_schemas) {
		return;
	}
	if (new_schemas->GetEntries().size() == 0) {
		throw InternalException("No schemas to create but new_schemas is set - should be cleaned up elsewhere");
	}
	string schema_insert_sql;
	for (auto &entry : new_schemas->GetEntries()) {
		auto &schema_entry = entry.second->Cast<DuckLakeSchemaEntry>();
		auto schema_id = commit_snapshot.next_catalog_id++;
		if (!schema_insert_sql.empty()) {
			schema_insert_sql += ",";
		}
		schema_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %s)", schema_id,
		                                        schema_entry.GetSchemaUUID(), SQLString(schema_entry.name));

		// set the schema id of this schema entry so subsequent tables are written correctly
		schema_entry.SetSchemaId(schema_id);
	}
	schema_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_schema VALUES " + schema_insert_sql;
	auto result = Query(commit_snapshot, schema_insert_sql);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new schemas to DuckLake: ");
	}
}

void DuckLakeTransaction::FlushNewPartitionKey(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table) {
	// set old partition data as no longer valid
	auto update_partition_query = StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_partition_info
SET end_snapshot = {SNAPSHOT_ID}
WHERE table_id = %d AND end_snapshot IS NULL)",
	                                                 table.GetTableId());
	auto result = Query(commit_snapshot, update_partition_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to update old partition information in DuckLake:");
	}
	// insert the new partition data
	auto partition_data = table.GetPartitionData();
	if (!partition_data) {
		// dropping partition data - we don't need to do anything
		return;
	}
	partition_data->partition_id = commit_snapshot.next_catalog_id++;
	auto new_partition_data_query = StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_partition_info
VALUES (%d, %d, {SNAPSHOT_ID}, NULL);)",
	                                                   partition_data->partition_id, table.GetTableId());
	result = Query(commit_snapshot, new_partition_data_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to insert new partition information in DuckLake:");
	}
	string insert_partition_cols;
	for (auto &field : partition_data->fields) {
		if (!insert_partition_cols.empty()) {
			insert_partition_cols += ", ";
		}
		if (field.transform.type != DuckLakeTransformType::IDENTITY) {
			throw NotImplementedException("FIXME: non-identity transform");
		}
		insert_partition_cols += StringUtil::Format("(%d, %d, %d, 'identity')", partition_data->partition_id,
		                                            field.partition_key_index, field.column_id);
	}
	insert_partition_cols = "INSERT INTO {METADATA_CATALOG}.ducklake_partition_columns VALUES " + insert_partition_cols;

	result = Query(commit_snapshot, insert_partition_cols);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to insert new partition information in DuckLake:");
	}
}

void DuckLakeTransaction::FlushNewTables(DuckLakeSnapshot &commit_snapshot) {
	if (new_tables.empty()) {
		return;
	}
	string table_insert_sql;
	string column_insert_sql;
	for (auto &schema_entry : new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			// write any new tables that we created
			auto &table = entry.second->Cast<DuckLakeTableEntry>();
			if (table.LocalChange() == TransactionLocalChange::SET_PARTITION_KEY) {
				FlushNewPartitionKey(commit_snapshot, table);
				continue;
			}
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
			if (!table_insert_sql.empty()) {
				table_insert_sql += ", ";
			}
			string table_uuid = table.GetTableUUID();
			table_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %d, %s)", table_id, table_uuid,
			                                       schema.GetSchemaId(), SQLString(table.name));
			if (is_new_table) {
				// if this is a new table - write the columns
				idx_t column_id = 0;
				for (auto &col : table.GetColumns().Logical()) {
					if (!column_insert_sql.empty()) {
						column_insert_sql += ", ";
					}
					column_insert_sql += StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, %d, %s, %s, NULL)",
					                                        column_id, table_id, column_id, SQLString(col.GetName()),
					                                        SQLString(DuckLakeTypes::ToString(col.GetType())));
					column_id++;
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
	if (!table_insert_sql.empty()) {
		// insert table entries
		table_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_table VALUES " + table_insert_sql;
		auto result = Query(commit_snapshot, table_insert_sql);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write new table to DuckLake: ");
		}
	}
	if (!column_insert_sql.empty()) {
		// insert column entries
		column_insert_sql = "INSERT INTO {METADATA_CATALOG}.ducklake_column VALUES " + column_insert_sql;
		auto result = Query(commit_snapshot, column_insert_sql);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write column information to DuckLake: ");
		}
	}
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

void DuckLakeTransaction::FlushNewData(DuckLakeSnapshot &commit_snapshot) {
	if (new_data_files.empty()) {
		return;
	}
	for (auto &entry : new_data_files) {
		string data_file_insert_query;
		string column_stats_insert_query;
		auto table_id = entry.first;
		if (IsTransactionLocal(table_id)) {
			throw InternalException("Cannot commit transaction local files - these should have been cleaned up before");
		}

		DuckLakeTableStats new_stats;
		for (auto &file : entry.second) {
			if (!data_file_insert_query.empty()) {
				data_file_insert_query += ",";
			}
			auto file_id = commit_snapshot.next_file_id++;
			data_file_insert_query += StringUtil::Format(
			    "(%d, %d, {SNAPSHOT_ID}, NULL, NULL, %s, 'parquet', %d, %d, %d, NULL)", file_id, table_id,
			    SQLString(file.file_name), file.row_count, file.file_size_bytes, file.footer_size);

			new_stats.record_count += file.row_count;
			new_stats.table_size_bytes += file.file_size_bytes;

			// gather the column statistics for this file
			for (auto &column_stats_entry : file.column_stats) {
				if (!column_stats_insert_query.empty()) {
					column_stats_insert_query += ",";
				}
				auto column_id = column_stats_entry.first;
				auto &stats = column_stats_entry.second;
				string min_val = stats.has_min ? DuckLakeUtil::SQLLiteralToString(stats.min) : "NULL";
				string max_val = stats.has_max ? DuckLakeUtil::SQLLiteralToString(stats.max) : "NULL";
				string value_count, null_count;
				if (stats.has_null_count) {
					value_count = to_string(file.row_count - stats.null_count);
					null_count = to_string(stats.null_count);
					if (stats.null_count == file.row_count) {
						// all values are NULL for this file
						stats.any_valid = false;
					}
				} else {
					value_count = "NULL";
					null_count = "NULL";
				}
				column_stats_insert_query +=
				    StringUtil::Format("(%d, %d, %d, NULL, %s, %s, NULL, %s, %s)", file_id, table_id, column_id,
				                       value_count, null_count, min_val, max_val);

				// merge the stats into the new global states
				new_stats.MergeStats(column_id, stats);
			}
		}
		if (data_file_insert_query.empty()) {
			throw InternalException("No files found!?");
		}
		// insert the data files
		data_file_insert_query =
		    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_data_file VALUES %s", data_file_insert_query);
		auto result = Query(commit_snapshot, data_file_insert_query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write data file information to DuckLake: ");
		}
		// insert the column stats
		column_stats_insert_query = StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_file_column_statistics VALUES %s", column_stats_insert_query);
		result = Query(commit_snapshot, column_stats_insert_query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to write column stats information to DuckLake: ");
		}
		// update the global stats for this table based on the newly written files
		UpdateGlobalTableStats(table_id, std::move(new_stats));
	}
}

void DuckLakeTransaction::InsertSnapshot(DuckLakeSnapshot commit_snapshot) {
	auto result = Query(
	    commit_snapshot,
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES ({SNAPSHOT_ID}, NOW(), {SCHEMA_VERSION}, {NEXT_CATALOG_ID}, {NEXT_FILE_ID});)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new snapshot to DuckLake: ");
	}
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
				metadata_manager->DropTables(commit_snapshot, dropped_schema_ids);
			}
			// write new schemas
			FlushNewSchemas(commit_snapshot);

			// write new tables
			FlushNewTables(commit_snapshot);

			// write new data files
			FlushNewData(commit_snapshot);

			// write the new snapshot
			InsertSnapshot(commit_snapshot);

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
