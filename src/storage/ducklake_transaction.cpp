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
      local_catalog_id(DuckLakeConstants::TRANSACTION_LOCAL_ID_START) {
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
	map<SchemaIndex, reference<DuckLakeSchemaEntry>> dropped_schemas;
	case_insensitive_map_t<reference_set_t<DuckLakeTableEntry>> created_tables;
	set<TableIndex> altered_tables;
	set<TableIndex> dropped_tables;
	set<TableIndex> inserted_tables;
};

struct SnapshotChangeInformation {
	case_insensitive_set_t created_schemas;
	set<SchemaIndex> dropped_schemas;
	case_insensitive_map_t<case_insensitive_set_t> created_tables;
	set<TableIndex> altered_tables;
	set<TableIndex> dropped_tables;
	set<TableIndex> inserted_tables;
	set<TableIndex> tables_deleted_from;
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
			reference<CatalogEntry> table_entry = *entry.second;
			while (true) {
				auto &table = table_entry.get().Cast<DuckLakeTableEntry>();
				if (table.LocalChange() == TransactionLocalChange::SET_PARTITION_KEY) {
					// this table was altered
					auto table_id = table.GetTableId();
					// don't report transaction-local tables yet - these will get added later on
					if (!table_id.IsTransactionLocal()) {
						changes.altered_tables.insert(table_id);
					}
				} else {
					// write any new tables that we created
					auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
					changes.created_tables[schema.name].insert(table);
				}

				if (!table_entry.get().HasChild()) {
					break;
				}
				table_entry = table_entry.get().Child();
			}
		}
	}
	for (auto &entry : new_data_files) {
		auto table_id = entry.first;
		if (table_id.IsTransactionLocal()) {
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
		auto schema_id = entry.first.index;
		change_info.schemas_dropped += to_string(schema_id);
	}
	for (auto &dropped_table_idx : changes.dropped_tables) {
		if (!change_info.tables_dropped.empty()) {
			change_info.tables_dropped += ",";
		}
		change_info.tables_dropped += to_string(dropped_table_idx.index);
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
		change_info.tables_inserted_into += to_string(inserted_table_idx.index);
	}
	for (auto &altered_table_idx : changes.altered_tables) {
		if (!change_info.tables_altered.empty()) {
			change_info.tables_altered += ",";
		}
		change_info.tables_altered += to_string(altered_table_idx.index);
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
			                           dropped_idx.index);
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
		auto dropped_entry = other_changes.dropped_tables.find(table_id);
		if (dropped_entry != other_changes.dropped_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to insert into table with id %d"
			                           "- but this table has been dropped by another transaction",
			                           table_id.index);
		}
		auto alter_entry = other_changes.altered_tables.find(table_id);
		if (alter_entry != other_changes.altered_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to insert into table with id %d"
			                           "- but this table has been altered by another transaction",
			                           table_id.index);
		}
	}
	for (auto &table_id : changes.altered_tables) {
		auto dropped_entry = other_changes.dropped_tables.find(table_id);
		if (dropped_entry != other_changes.dropped_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to alter table with id %d"
			                           "- but this table has been dropped by another transaction",
			                           table_id.index);
		}
		auto alter_entry = other_changes.altered_tables.find(table_id);
		if (alter_entry != other_changes.altered_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to alter table with id %d"
			                           "- but this table has been altered by another transaction",
			                           table_id.index);
		}
	}
}

template <class T>
set<T> ParseDropList(const string &input) {
	set<T> result;
	if (input.empty()) {
		return result;
	}
	auto splits = StringUtil::Split(input, ",");
	for (auto &split : splits) {
		result.insert(T(std::stoull(split)));
	}
	return result;
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
	other_changes.dropped_schemas = ParseDropList<SchemaIndex>(changes_made.schemas_dropped);
	other_changes.dropped_tables = ParseDropList<TableIndex>(changes_made.tables_dropped);
	other_changes.altered_tables = ParseDropList<TableIndex>(changes_made.tables_altered);
	other_changes.inserted_tables = ParseDropList<TableIndex>(changes_made.tables_inserted_into);
	other_changes.tables_deleted_from = ParseDropList<TableIndex>(changes_made.tables_deleted_from);
	CheckForConflicts(changes, other_changes);
}

vector<DuckLakeSchemaInfo> DuckLakeTransaction::GetNewSchemas(DuckLakeSnapshot &commit_snapshot) {
	vector<DuckLakeSchemaInfo> schemas;
	for (auto &entry : new_schemas->GetEntries()) {
		auto &schema_entry = entry.second->Cast<DuckLakeSchemaEntry>();
		DuckLakeSchemaInfo schema_info;
		schema_info.id = SchemaIndex(commit_snapshot.next_catalog_id++);
		schema_info.uuid = schema_entry.GetSchemaUUID();
		schema_info.name = schema_entry.name;

		// set the schema id of this schema entry so subsequent tables are written correctly
		schema_entry.SetSchemaId(schema_info.id);

		// add the schema to the list
		schemas.push_back(std::move(schema_info));
	}
	return schemas;
}

DuckLakePartitionInfo DuckLakeTransaction::GetNewPartitionKey(DuckLakeSnapshot &commit_snapshot,
                                                              DuckLakeTableEntry &table) {
	DuckLakePartitionInfo partition_key;
	partition_key.table_id = table.GetTableId();
	if (partition_key.table_id.IsTransactionLocal()) {
		throw InternalException("Trying to write partition with transaction local table-id");
	}
	// insert the new partition data
	auto partition_data = table.GetPartitionData();
	if (!partition_data) {
		// dropping partition data - insert the empty partition key data for this table
		return partition_key;
	}
	auto local_partition_id = partition_data->partition_id;
	auto partition_id = commit_snapshot.next_catalog_id++;
	partition_key.id = partition_id;
	partition_data->partition_id = partition_id;
	for (auto &field : partition_data->fields) {
		DuckLakePartitionFieldInfo partition_field;
		partition_field.partition_key_index = field.partition_key_index;
		partition_field.column_id = field.column_id;
		if (field.transform.type != DuckLakeTransformType::IDENTITY) {
			throw NotImplementedException("Unimplemented transform type for partition");
		}
		partition_field.transform = "identity";
		partition_key.fields.push_back(std::move(partition_field));
	}

	// if we wrote any data with this partition id - rewrite it to the latest partition id
	for (auto &entry : new_data_files) {
		for (auto &file : entry.second) {
			if (file.partition_id.IsValid() && file.partition_id.GetIndex() == local_partition_id) {
				file.partition_id = partition_id;
			}
		}
	}
	return partition_key;
}

DuckLakeColumnInfo ConvertColumn(const string &name, const LogicalType &type, const DuckLakeFieldId &field_id) {
	DuckLakeColumnInfo column_entry;
	column_entry.id = field_id.GetFieldIndex();
	column_entry.name = name;
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		column_entry.type = "struct";
		auto &struct_children = StructType::GetChildTypes(type);
		for (idx_t child_idx = 0; child_idx < struct_children.size(); ++child_idx) {
			auto &child = struct_children[child_idx];
			auto &child_id = field_id.GetChildByIndex(child_idx);
			column_entry.children.push_back(ConvertColumn(child.first, child.second, child_id));
		}
		break;
	}
	case LogicalTypeId::LIST: {
		column_entry.type = "list";
		auto &child_id = field_id.GetChildByIndex(0);
		column_entry.children.push_back(ConvertColumn("element", ListType::GetChildType(type), child_id));
		break;
	}
	case LogicalTypeId::ARRAY: {
		column_entry.type = "list";
		auto &child_id = field_id.GetChildByIndex(0);
		column_entry.children.push_back(ConvertColumn("element", ArrayType::GetChildType(type), child_id));
		break;
	}
	case LogicalTypeId::MAP: {
		column_entry.type = "map";
		auto &key_id = field_id.GetChildByIndex(0);
		auto &value_id = field_id.GetChildByIndex(1);
		column_entry.children.push_back(ConvertColumn("key", MapType::KeyType(type), key_id));
		column_entry.children.push_back(ConvertColumn("value", MapType::ValueType(type), value_id));
		break;
	}
	default:
		column_entry.type = DuckLakeTypes::ToString(type);
		break;
	}
	return column_entry;
}

DuckLakeTableInfo DuckLakeTransaction::GetNewTable(DuckLakeSnapshot &commit_snapshot, DuckLakeTableEntry &table) {
	auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
	DuckLakeTableInfo table_entry;
	auto original_id = table.GetTableId();
	bool is_new_table;
	if (original_id.IsTransactionLocal()) {
		table_entry.id = TableIndex(commit_snapshot.next_catalog_id++);
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
		for (auto &col : table.GetColumns().Logical()) {
			table_entry.columns.push_back(
			    ConvertColumn(col.GetName(), col.GetType(), table.GetFieldId(col.Physical())));
		}
		// if we have written any data to this table - move them to the new (correct) table id as well
		auto data_file_entry = new_data_files.find(original_id);
		if (data_file_entry != new_data_files.end()) {
			new_data_files[table_entry.id] = std::move(data_file_entry->second);
			new_data_files.erase(original_id);
		}
	}
	return table_entry;
}

vector<DuckLakeTableInfo> DuckLakeTransaction::GetNewTables(DuckLakeSnapshot &commit_snapshot,
                                                            vector<DuckLakePartitionInfo> &new_partition_keys,
                                                            TransactionChangeInformation &transaction_changes) {
	vector<DuckLakeTableInfo> result;
	for (auto &schema_entry : new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			// iterate over the table chain in reverse order when committing
			// the latest entry is the root entry - but we need to commit starting from the first entry written
			reference<CatalogEntry> table_entry = *entry.second;
			// gather all tables
			vector<reference<DuckLakeTableEntry>> tables;
			while (true) {
				tables.push_back(table_entry.get().Cast<DuckLakeTableEntry>());
				if (!table_entry.get().HasChild()) {
					break;
				}
				table_entry = table_entry.get().Child();
			}
			// traverse in reverse order
			TableIndex new_table_id;
			for (idx_t table_idx = tables.size(); table_idx > 0; table_idx--) {
				auto &table = tables[table_idx - 1].get();
				if (new_table_id.IsValid()) {
					table.SetTableId(new_table_id);
				}
				if (table.LocalChange() == TransactionLocalChange::SET_PARTITION_KEY) {
					auto partition_key = GetNewPartitionKey(commit_snapshot, table);
					new_partition_keys.push_back(std::move(partition_key));

					transaction_changes.altered_tables.insert(table.GetTableId());
				} else {
					auto new_table = GetNewTable(commit_snapshot, table);
					new_table_id = new_table.id;
					result.push_back(std::move(new_table));
				}
			}
		}
	}
	return result;
}

void DuckLakeTransaction::UpdateGlobalTableStats(TableIndex table_id, DuckLakeTableStats new_stats) {
	DuckLakeGlobalStatsInfo stats;
	stats.table_id = table_id;

	auto current_stats = ducklake_catalog.GetTableStats(*this, table_id);
	stats.initialized = false;
	if (current_stats) {
		// merge the current stats into the new stats
		for (auto &entry : current_stats->column_stats) {
			new_stats.MergeStats(entry.first, entry.second);
		}
		new_stats.record_count += current_stats->record_count;
		new_stats.table_size_bytes += current_stats->table_size_bytes;
		stats.initialized = true;
	}
	for (auto &entry : new_stats.column_stats) {
		DuckLakeGlobalColumnStatsInfo col_stats;
		col_stats.column_id = entry.first;
		auto &column_stats = entry.second;
		col_stats.has_contains_null = column_stats.has_null_count;
		if (column_stats.has_null_count) {
			col_stats.contains_null = column_stats.null_count > 0;
		}
		col_stats.has_min = column_stats.has_min;
		if (column_stats.has_min) {
			col_stats.min_val = column_stats.min;
		}
		col_stats.has_max = column_stats.has_max;
		if (column_stats.has_max) {
			col_stats.max_val = column_stats.max;
		}
		stats.column_stats.push_back(std::move(col_stats));
	}
	stats.record_count = new_stats.record_count;
	stats.table_size_bytes = new_stats.table_size_bytes;
	// finally update the stats in the tables
	metadata_manager->UpdateGlobalTableStats(stats);
}

vector<DuckLakeFileInfo> DuckLakeTransaction::GetNewDataFiles(DuckLakeSnapshot &commit_snapshot) {
	vector<DuckLakeFileInfo> result;
	for (auto &entry : new_data_files) {
		auto table_id = entry.first;
		if (table_id.IsTransactionLocal()) {
			throw InternalException("Cannot commit transaction local files - these should have been cleaned up before");
		}

		DuckLakeTableStats new_stats;
		for (auto &file : entry.second) {
			DuckLakeFileInfo data_file;
			data_file.id = DataFileIndex(commit_snapshot.next_file_id++);
			data_file.table_id = table_id;
			data_file.file_name = file.file_name;
			data_file.row_count = file.row_count;
			data_file.file_size_bytes = file.file_size_bytes;
			data_file.footer_size = file.footer_size;
			data_file.partition_id = file.partition_id;

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
				set<SchemaIndex> dropped_schema_ids;
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
				auto table_list = GetNewTables(commit_snapshot, partition_keys, transaction_changes);
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
				error.Throw("Failed to commit DuckLake transaction: ");
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

DuckLakeSnapshot DuckLakeTransaction::GetSnapshot(optional_ptr<BoundAtClause> at_clause) {
	if (!at_clause) {
		// no AT-clause - get the latest snapshot
		return GetSnapshot();
	}
	return *metadata_manager->GetSnapshot(*at_clause);
}

idx_t DuckLakeTransaction::GetLocalCatalogId() {
	return local_catalog_id++;
}

vector<DuckLakeDataFile> DuckLakeTransaction::GetTransactionLocalFiles(TableIndex table_id) {
	auto entry = new_data_files.find(table_id);
	if (entry == new_data_files.end()) {
		return vector<DuckLakeDataFile>();
	} else {
		return entry->second;
	}
}

void DuckLakeTransaction::AppendFiles(TableIndex table_id, const vector<DuckLakeDataFile> &files) {
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
	if (schema_id.IsTransactionLocal()) {
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
