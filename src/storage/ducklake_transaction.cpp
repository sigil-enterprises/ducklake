#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_view_entry.hpp"
#include "common/ducklake_types.hpp"
#include "common/ducklake_util.hpp"

#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "storage/ducklake_transaction_changes.hpp"

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
	return !new_tables.empty() || !dropped_tables.empty() || new_schemas || !dropped_schemas.empty() ||
	       !dropped_views.empty();
}

bool DuckLakeTransaction::ChangesMade() {
	return SchemaChangesMade() || !new_data_files.empty() || !new_delete_files.empty();
}

struct TransactionChangeInformation {
	case_insensitive_set_t created_schemas;
	map<SchemaIndex, reference<DuckLakeSchemaEntry>> dropped_schemas;
	case_insensitive_map_t<reference_set_t<CatalogEntry>> created_tables;
	set<TableIndex> altered_tables;
	set<TableIndex> altered_views;
	set<TableIndex> dropped_tables;
	set<TableIndex> dropped_views;
	set<TableIndex> inserted_tables;
};

void GetTransactionTableChanges(reference<CatalogEntry> table_entry, TransactionChangeInformation &changes) {
	while (true) {
		auto &table = table_entry.get().Cast<DuckLakeTableEntry>();
		switch (table.GetLocalChange().type) {
		case LocalChangeType::SET_PARTITION_KEY:
		case LocalChangeType::SET_COMMENT:
		case LocalChangeType::SET_COLUMN_COMMENT:
		case LocalChangeType::SET_NULL:
		case LocalChangeType::DROP_NULL:
		case LocalChangeType::RENAME_COLUMN:
		case LocalChangeType::ADD_COLUMN:
		case LocalChangeType::REMOVE_COLUMN:
		case LocalChangeType::CHANGE_COLUMN_TYPE:
		case LocalChangeType::SET_DEFAULT: {
			// this table was altered
			auto table_id = table.GetTableId();
			// don't report transaction-local tables yet - these will get added later on
			if (!table_id.IsTransactionLocal()) {
				changes.altered_tables.insert(table_id);
			}
			break;
		}
		case LocalChangeType::NONE:
		case LocalChangeType::CREATED:
		case LocalChangeType::RENAMED: {
			// write any new tables that we created
			auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
			changes.created_tables[schema.name].insert(table);
			break;
		}
		default:
			throw NotImplementedException("Unsupported transaction local change in GetTransactionTableChanges");
		}
		if (!table_entry.get().HasChild()) {
			break;
		}
		table_entry = table_entry.get().Child();
	}
}

void GetTransactionViewChanges(reference<CatalogEntry> view_entry, TransactionChangeInformation &changes) {
	while (true) {
		auto &view = view_entry.get().Cast<DuckLakeViewEntry>();
		switch (view.GetLocalChange().type) {
		case LocalChangeType::SET_COMMENT: {
			// this table was altered
			auto view_id = view.GetViewId();
			// don't report transaction-local views yet - these will get added later on
			if (!view_id.IsTransactionLocal()) {
				changes.altered_views.insert(view_id);
			}
			break;
		}
		case LocalChangeType::NONE:
		case LocalChangeType::CREATED:
		case LocalChangeType::RENAMED: {
			// write any new view that we created
			auto &schema = view.ParentSchema().Cast<DuckLakeSchemaEntry>();
			changes.created_tables[schema.name].insert(view);
			break;
		}
		default:
			throw NotImplementedException("Unsupported transaction local change in GetTransactionTableChanges");
		}

		if (!view_entry.get().HasChild()) {
			break;
		}
		view_entry = view_entry.get().Child();
	}
}

TransactionChangeInformation DuckLakeTransaction::GetTransactionChanges() {
	TransactionChangeInformation changes;
	for (auto &dropped_table_idx : dropped_tables) {
		changes.dropped_tables.insert(dropped_table_idx);
	}
	for (auto &dropped_view_idx : dropped_views) {
		changes.dropped_views.insert(dropped_view_idx);
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
			switch (entry.second->type) {
			case CatalogType::TABLE_ENTRY:
				GetTransactionTableChanges(*entry.second, changes);
				break;
			case CatalogType::VIEW_ENTRY:
				GetTransactionViewChanges(*entry.second, changes);
				break;
			default:
				throw InternalException("Unsupported type found in new_tables");
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
		if (!change_info.changes_made.empty()) {
			change_info.changes_made += ",";
		}
		auto schema_id = entry.first.index;
		change_info.changes_made += "dropped_schema:";
		change_info.changes_made += to_string(schema_id);
	}
	for (auto &dropped_table_idx : changes.dropped_tables) {
		if (!change_info.changes_made.empty()) {
			change_info.changes_made += ",";
		}
		change_info.changes_made += "dropped_table:";
		change_info.changes_made += to_string(dropped_table_idx.index);
	}
	for (auto &dropped_view_idx : changes.dropped_views) {
		if (!change_info.changes_made.empty()) {
			change_info.changes_made += ",";
		}
		change_info.changes_made += "dropped_view:";
		change_info.changes_made += to_string(dropped_view_idx.index);
	}
	for (auto &created_schema : changes.created_schemas) {
		if (!change_info.changes_made.empty()) {
			change_info.changes_made += ",";
		}
		change_info.changes_made += "created_schema:";
		change_info.changes_made += KeywordHelper::WriteQuoted(created_schema, '"');
	}
	for (auto &entry : changes.created_tables) {
		auto &schema = entry.first;
		auto schema_prefix = KeywordHelper::WriteQuoted(schema, '"') + ".";
		for (auto &created_table : entry.second) {
			if (!change_info.changes_made.empty()) {
				change_info.changes_made += ",";
			}
			auto is_view = created_table.get().type == CatalogType::VIEW_ENTRY;
			change_info.changes_made += is_view ? "created_view:" : "created_table:";
			change_info.changes_made += schema_prefix + KeywordHelper::WriteQuoted(created_table.get().name, '"');
		}
	}
	for (auto &inserted_table_idx : changes.inserted_tables) {
		if (!change_info.changes_made.empty()) {
			change_info.changes_made += ",";
		}
		change_info.changes_made += "inserted_into_table:";
		change_info.changes_made += to_string(inserted_table_idx.index);
	}
	for (auto &altered_table_idx : changes.altered_tables) {
		if (!change_info.changes_made.empty()) {
			change_info.changes_made += ",";
		}
		change_info.changes_made += "altered_table:";
		change_info.changes_made += to_string(altered_table_idx.index);
	}
	for (auto &altered_view_idx : changes.altered_views) {
		if (!change_info.changes_made.empty()) {
			change_info.changes_made += ",";
		}
		change_info.changes_made += "altered_view:";
		change_info.changes_made += to_string(altered_view_idx.index);
	}
	metadata_manager->WriteSnapshotChanges(commit_snapshot, change_info);
}

void DuckLakeTransaction::CleanupFiles() {
	// remove any files that were written
	auto &fs = FileSystem::GetFileSystem(db);
	for (auto &table_entry : new_data_files) {
		for (auto &file : *table_entry.second) {
			fs.RemoveFile(file.file_name);
			if (file.delete_file) {
				fs.RemoveFile(file.delete_file->file_name);
			}
		}
	}
	for (auto &table_entry : new_delete_files) {
		for (auto &file : table_entry.second) {
			fs.RemoveFile(file.second.file_name);
		}
	}
	new_data_files.clear();
	new_delete_files.clear();
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
		auto tbl_entry = other_changes.created_tables.find(dropped_schema.name);
		if (tbl_entry != other_changes.created_tables.end()) {
			throw TransactionException("Transaction conflict - attempting to drop schema \"%s\""
			                           "- but another transaction has created a an entry in this schema",
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
			auto entry_type = table.type == CatalogType::TABLE_ENTRY ? "table" : "view";
			auto schema_entry = other_changes.dropped_schemas.find(schema.GetSchemaId());
			if (schema_entry != other_changes.dropped_schemas.end()) {
				// the schema this table was created in was dropped
				throw TransactionException("Transaction conflict - attempting to create %s \"%s\" in schema \"%s\" "
				                           "- but this schema has been dropped by another transaction already",
				                           entry_type, table.name, schema_name);
			}
			auto tbl_entry = other_changes.created_tables.find(schema_name);
			if (tbl_entry != other_changes.created_tables.end()) {
				auto &other_created_tables = tbl_entry->second;
				auto sub_entry = other_created_tables.find(table.name);
				if (sub_entry != other_created_tables.end()) {
					// a table with this name in this schema was already created
					throw TransactionException("Transaction conflict - attempting to create %s \"%s\" in schema \"%s\" "
					                           "- but this %s has been created by another transaction already",
					                           entry_type, table.name, schema_name, sub_entry->second);
				}
			}
		}
	}
	for (auto &table_id : changes.inserted_tables) {
		auto dropped_entry = other_changes.dropped_tables.find(table_id);
		if (dropped_entry != other_changes.dropped_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to insert into table with id %d"
			                           " - but this table has been dropped by another transaction",
			                           table_id.index);
		}
		auto alter_entry = other_changes.altered_tables.find(table_id);
		if (alter_entry != other_changes.altered_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to insert into table with id %d"
			                           " - but this table has been altered by another transaction",
			                           table_id.index);
		}
	}
	for (auto &table_id : changes.altered_tables) {
		auto dropped_entry = other_changes.dropped_tables.find(table_id);
		if (dropped_entry != other_changes.dropped_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to alter table with id %d"
			                           " - but this table has been dropped by another transaction",
			                           table_id.index);
		}
		auto alter_entry = other_changes.altered_tables.find(table_id);
		if (alter_entry != other_changes.altered_tables.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to alter table with id %d"
			                           " - but this table has been altered by another transaction",
			                           table_id.index);
		}
	}
	for (auto &view_id : changes.altered_views) {
		auto alter_entry = other_changes.altered_views.find(view_id);
		if (alter_entry != other_changes.altered_views.end()) {
			// trying to insert into a table that was dropped
			throw TransactionException("Transaction conflict - attempting to alter view with id %d"
			                           " - but this view has been altered by another transaction",
			                           view_id.index);
		}
	}
}

void DuckLakeTransaction::CheckForConflicts(DuckLakeSnapshot transaction_snapshot,
                                            const TransactionChangeInformation &changes) {

	// get all changes made to the system after the current snapshot was started
	auto changes_made = metadata_manager->GetChangesMadeAfterSnapshot(transaction_snapshot);
	// parse changes made by other transactions
	auto other_changes = SnapshotChangeInformation::ParseChangesMade(changes_made.changes_made);

	// now check for conflicts
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
		for (auto &file : *entry.second) {
			if (file.partition_id.IsValid() && file.partition_id.GetIndex() == local_partition_id) {
				file.partition_id = partition_id;
			}
		}
	}
	return partition_key;
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
		auto not_null_fields = table.GetNotNullFields();
		for (auto &col : table.GetColumns().Logical()) {
			auto col_info =
			    DuckLakeTableEntry::ConvertColumn(col.GetName(), col.GetType(), table.GetFieldId(col.Physical()));
			if (not_null_fields.count(col.GetName())) {
				// no null values allowed in this field
				col_info.nulls_allowed = false;
			}
			table_entry.columns.push_back(std::move(col_info));
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

struct NewTableInfo {
	vector<DuckLakeTableInfo> new_tables;
	vector<DuckLakeViewInfo> new_views;
	vector<DuckLakePartitionInfo> new_partition_keys;
	vector<DuckLakeTagInfo> new_tags;
	vector<DuckLakeColumnTagInfo> new_column_tags;
	vector<DuckLakeDroppedColumn> dropped_columns;
	vector<DuckLakeNewColumn> new_columns;
};

void HandleChangedFields(TableIndex table_id, const ColumnChangeInfo &change_info, NewTableInfo &result) {
	for (auto &new_col_info : change_info.new_fields) {
		DuckLakeNewColumn new_column;
		new_column.table_id = table_id;
		new_column.column_info = new_col_info.column_info;
		new_column.parent_idx = new_col_info.parent_idx;
		result.new_columns.push_back(std::move(new_column));
	}
	for (auto &dropped_field_id : change_info.dropped_fields) {
		DuckLakeDroppedColumn dropped_col;
		dropped_col.table_id = table_id;
		dropped_col.field_id = dropped_field_id;
		result.dropped_columns.push_back(dropped_col);
	}
}

void DuckLakeTransaction::GetNewTableInfo(DuckLakeSnapshot &commit_snapshot, reference<CatalogEntry> table_entry,
                                          NewTableInfo &result, TransactionChangeInformation &transaction_changes) {
	// iterate over the table chain in reverse order when committing
	// the latest entry is the root entry - but we need to commit starting from the first entry written
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
		auto local_change = table.GetLocalChange();
		switch (local_change.type) {
		case LocalChangeType::SET_PARTITION_KEY: {
			auto partition_key = GetNewPartitionKey(commit_snapshot, table);
			result.new_partition_keys.push_back(std::move(partition_key));

			transaction_changes.altered_tables.insert(table.GetTableId());
			break;
		}
		case LocalChangeType::SET_COMMENT: {
			DuckLakeTagInfo comment_info;
			comment_info.id = table.GetTableId().index;
			comment_info.key = "comment";
			comment_info.value = table.comment;
			result.new_tags.push_back(std::move(comment_info));

			transaction_changes.altered_tables.insert(table.GetTableId());
			break;
		}
		case LocalChangeType::SET_COLUMN_COMMENT: {
			DuckLakeColumnTagInfo comment_info;
			comment_info.table_id = table.GetTableId();
			comment_info.field_index = local_change.field_index;
			comment_info.key = "comment";
			comment_info.value = table.GetColumnByFieldId(local_change.field_index).Comment();
			result.new_column_tags.push_back(std::move(comment_info));

			transaction_changes.altered_tables.insert(table.GetTableId());
			break;
		}
		case LocalChangeType::SET_NULL:
		case LocalChangeType::DROP_NULL:
		case LocalChangeType::RENAME_COLUMN:
		case LocalChangeType::SET_DEFAULT: {
			// drop the previous column
			DuckLakeDroppedColumn dropped_col;
			dropped_col.table_id = table.GetTableId();
			dropped_col.field_id = local_change.field_index;
			result.dropped_columns.push_back(dropped_col);

			// insert the new column with the new info
			DuckLakeNewColumn new_col;
			new_col.table_id = table.GetTableId();
			new_col.column_info = table.GetColumnInfo(local_change.field_index);
			result.new_columns.push_back(std::move(new_col));

			transaction_changes.altered_tables.insert(table.GetTableId());
			break;
		}
		case LocalChangeType::REMOVE_COLUMN:
		case LocalChangeType::CHANGE_COLUMN_TYPE: {
			// drop the indicated column
			// note that in case of nested types we might be dropping multiple columns here
			HandleChangedFields(table.GetTableId(), table.GetChangedFields(), result);
			break;
		}
		case LocalChangeType::ADD_COLUMN: {
			// insert the new column
			DuckLakeNewColumn new_col;
			new_col.table_id = table.GetTableId();
			new_col.column_info = table.GetAddColumnInfo();
			result.new_columns.push_back(std::move(new_col));

			transaction_changes.altered_tables.insert(table.GetTableId());
			break;
		}
		case LocalChangeType::NONE:
		case LocalChangeType::CREATED:
		case LocalChangeType::RENAMED: {
			auto new_table = GetNewTable(commit_snapshot, table);
			new_table_id = new_table.id;
			result.new_tables.push_back(std::move(new_table));
			break;
		}
		default:
			throw NotImplementedException("Unsupported transaction local change");
		}
	}
}

DuckLakeViewInfo DuckLakeTransaction::GetNewView(DuckLakeSnapshot &commit_snapshot, DuckLakeViewEntry &view) {
	auto &schema = view.ParentSchema().Cast<DuckLakeSchemaEntry>();
	DuckLakeViewInfo view_entry;
	auto original_id = view.GetViewId();
	if (original_id.IsTransactionLocal()) {
		view_entry.id = TableIndex(commit_snapshot.next_catalog_id++);
	} else {
		// this view already has an id - keep it
		// this happens if e.g. this view is renamed
		view_entry.id = original_id;
	}
	view_entry.uuid = view.GetViewUUID();
	view_entry.schema_id = schema.GetSchemaId();
	view_entry.name = view.name;
	view_entry.dialect = "duckdb";
	view_entry.sql = view.GetQuerySQL();
	view_entry.column_aliases = view.aliases;
	return view_entry;
}

void DuckLakeTransaction::GetNewViewInfo(DuckLakeSnapshot &commit_snapshot, reference<CatalogEntry> view_entry,
                                         NewTableInfo &result, TransactionChangeInformation &transaction_changes) {
	// iterate over the view chain in reverse order when committing
	// the latest entry is the root entry - but we need to commit starting from the first entry written
	// gather all views
	vector<reference<DuckLakeViewEntry>> views;
	while (true) {
		views.push_back(view_entry.get().Cast<DuckLakeViewEntry>());
		if (!view_entry.get().HasChild()) {
			break;
		}
		view_entry = view_entry.get().Child();
	}
	// traverse in reverse order
	TableIndex new_view_id;
	for (idx_t view_idx = views.size(); view_idx > 0; view_idx--) {
		auto &view = views[view_idx - 1].get();
		if (new_view_id.IsValid()) {
			view.SetViewId(new_view_id);
		}
		switch (view.GetLocalChange().type) {
		case LocalChangeType::SET_COMMENT: {
			DuckLakeTagInfo comment_info;
			comment_info.id = view.GetViewId().index;
			comment_info.key = "comment";
			comment_info.value = view.comment;
			result.new_tags.push_back(std::move(comment_info));

			transaction_changes.altered_views.insert(view.GetViewId());
			break;
		}
		case LocalChangeType::NONE:
		case LocalChangeType::CREATED:
		case LocalChangeType::RENAMED: {
			auto new_view = GetNewView(commit_snapshot, view);
			new_view_id = new_view.id;
			result.new_views.push_back(std::move(new_view));
			break;
		}
		default:
			throw NotImplementedException("Unsupported transaction local change");
		}
	}
}

NewTableInfo DuckLakeTransaction::GetNewTables(DuckLakeSnapshot &commit_snapshot,
                                               TransactionChangeInformation &transaction_changes) {
	NewTableInfo result;
	for (auto &schema_entry : new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			switch (entry.second->type) {
			case CatalogType::TABLE_ENTRY:
				GetNewTableInfo(commit_snapshot, *entry.second, result, transaction_changes);
				break;
			case CatalogType::VIEW_ENTRY:
				GetNewViewInfo(commit_snapshot, *entry.second, result, transaction_changes);
				break;
			default:
				throw InternalException("Unknown type in new_tables");
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
		col_stats.has_contains_nan = column_stats.has_contains_nan;
		if (column_stats.has_contains_nan) {
			col_stats.contains_nan = column_stats.contains_nan;
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
		vector<DuckLakeDeleteFile> delete_files;
		for (auto &file : *entry.second) {
			DuckLakeFileInfo data_file;
			data_file.id = DataFileIndex(commit_snapshot.next_file_id++);
			data_file.table_id = table_id;
			data_file.file_name = file.file_name;
			data_file.row_count = file.row_count;
			data_file.file_size_bytes = file.file_size_bytes;
			data_file.footer_size = file.footer_size;
			data_file.partition_id = file.partition_id;
			if (file.delete_file) {
				// this transaction-local file already has deletes - write them out
				DuckLakeDeleteFile delete_file = *file.delete_file;
				delete_file.data_file_id = data_file.id;
				delete_files.push_back(std::move(delete_file));
			}

			new_stats.record_count += file.row_count;
			new_stats.table_size_bytes += file.file_size_bytes;

			// gather the column statistics for this file
			for (auto &column_stats_entry : file.column_stats) {
				DuckLakeColumnStatsInfo column_stats;
				column_stats.column_id = column_stats_entry.first;
				auto &stats = column_stats_entry.second;
				column_stats.min_val = stats.has_min ? DuckLakeUtil::SQLLiteralToString(stats.min) : "NULL";
				column_stats.max_val = stats.has_max ? DuckLakeUtil::SQLLiteralToString(stats.max) : "NULL";
				column_stats.column_size_bytes = to_string(stats.column_size_bytes);
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
				if (stats.has_contains_nan) {
					column_stats.contains_nan = stats.contains_nan ? "true" : "false";
				} else {
					column_stats.contains_nan = "NULL";
				}

				// merge the stats into the new global states
				new_stats.MergeStats(column_stats.column_id, stats);

				data_file.column_stats.push_back(std::move(column_stats));
			}
			for (auto &partition_entry : file.partition_values) {
				DuckLakeFilePartitionInfo partition_info;
				partition_info.partition_column_idx = partition_entry.partition_column_idx;
				partition_info.partition_value = partition_entry.partition_value;
				data_file.partition_values.push_back(std::move(partition_info));
			}
			result.push_back(std::move(data_file));
		}
		// write any deletes that were made on top of these transaction-local files
		AddDeletes(table_id, std::move(delete_files));

		// update the global stats for this table based on the newly written files
		UpdateGlobalTableStats(table_id, std::move(new_stats));
	}
	return result;
}

vector<DuckLakeDeleteFileInfo> DuckLakeTransaction::GetNewDeleteFiles(DuckLakeSnapshot &commit_snapshot, set<DataFileIndex> &overwritten_delete_files) {
	vector<DuckLakeDeleteFileInfo> result;
	for(auto &entry : new_delete_files) {
		auto table_id = entry.first;
		for(auto &file_entry : entry.second) {
			auto &file = file_entry.second;
			if (file.overwrites_existing_delete) {
				overwritten_delete_files.insert(file.data_file_id);
			}
			DuckLakeDeleteFileInfo delete_file;
			delete_file.id = DataFileIndex(commit_snapshot.next_file_id++);
			delete_file.table_id = table_id;
			delete_file.data_file_id = file.data_file_id;
			delete_file.path = file.file_name;
			delete_file.delete_count = file.delete_count;
			delete_file.file_size_bytes = file.file_size_bytes;
			delete_file.footer_size = file.footer_size;
			result.push_back(std::move(delete_file));
		}
	}
	return result;
}

void DuckLakeTransaction::FlushChanges() {
	if (!ChangesMade()) {
		// read-only transactions don't need to do anything
		return;
	}
	// FIXME: this should probably be configurable
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
			if (!dropped_views.empty()) {
				metadata_manager->DropViews(commit_snapshot, dropped_views);
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
				auto result = GetNewTables(commit_snapshot, transaction_changes);
				metadata_manager->WriteNewTables(commit_snapshot, result.new_tables);
				metadata_manager->WriteNewPartitionKeys(commit_snapshot, result.new_partition_keys);
				metadata_manager->WriteNewViews(commit_snapshot, result.new_views);
				metadata_manager->WriteNewTags(commit_snapshot, result.new_tags);
				metadata_manager->WriteNewColumnTags(commit_snapshot, result.new_column_tags);
				metadata_manager->WriteDroppedColumns(commit_snapshot, result.dropped_columns);
				metadata_manager->WriteNewColumns(commit_snapshot, result.new_columns);
			}

			// write new data files
			if (!new_data_files.empty()) {
				auto file_list = GetNewDataFiles(commit_snapshot);
				metadata_manager->WriteNewDataFiles(commit_snapshot, file_list);
			}

			if (!new_delete_files.empty()) {
				set<DataFileIndex> overwritten_delete_files;
				auto file_list = GetNewDeleteFiles(commit_snapshot, overwritten_delete_files);
				metadata_manager->DropDeleteFiles(commit_snapshot, overwritten_delete_files);
				metadata_manager->WriteNewDeleteFiles(commit_snapshot, file_list);
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
	// construct a struct value from the AT clause in the form of {"unit": value} (e.g. {"version": 2}
	// this is used as a caching key for the snapshot
	child_list_t<Value> values;
	values.push_back(make_pair(at_clause->Unit(), at_clause->GetValue()));
	auto snapshot_value = Value::STRUCT(std::move(values));
	auto entry = snapshot_cache.find(snapshot_value);
	if (entry != snapshot_cache.end()) {
		// we already found this snapshot - return it
		return entry->second;
	}
	// find the snapshot and cache it
	auto result_snapshot = *metadata_manager->GetSnapshot(*at_clause);
	snapshot_cache.insert(make_pair(std::move(snapshot_value), result_snapshot));
	return result_snapshot;
}

idx_t DuckLakeTransaction::GetLocalCatalogId() {
	return local_catalog_id++;
}

bool DuckLakeTransaction::HasTransactionLocalChanges(TableIndex table_id) {
	return new_data_files.find(table_id) != new_data_files.end();
}

optional_ptr<vector<DuckLakeDataFile>> DuckLakeTransaction::GetTransactionLocalFiles(TableIndex table_id) {
	auto entry = new_data_files.find(table_id);
	if (entry == new_data_files.end()) {
		return nullptr;
	} else {
		return entry->second.get();
	}
}

void DuckLakeTransaction::AppendFiles(TableIndex table_id, vector<DuckLakeDataFile> files) {
	auto entry = new_data_files.find(table_id);
	if (entry != new_data_files.end()) {
		// already exists - append
		auto &existing_files = entry->second;
		for(auto &file : files) {
			existing_files->push_back(std::move(file));
		}
	} else {
		auto file_list = make_uniq<vector<DuckLakeDataFile>>();
		for(auto &file : files) {
			file_list->push_back(std::move(file));
		}
		new_data_files.insert(make_pair(table_id, std::move(file_list)));
	}
}

void DuckLakeTransaction::AddDeletes(TableIndex table_id, vector<DuckLakeDeleteFile> files) {
	if (files.empty()) {
		return;
	}
	auto &table_delete_map = new_delete_files[table_id];
	for(auto &file : files) {
		auto &data_file_path = file.data_file_path;
		if (data_file_path.empty()) {
			throw InternalException("Data file path needs to be set in delete");
		}
		auto existing_entry = table_delete_map.find(data_file_path);
		if (existing_entry != table_delete_map.end()) {
			// we have a transaction-local delete for this file already - delete it
			auto &fs = FileSystem::GetFileSystem(db);
			fs.RemoveFile(existing_entry->second.file_name);
			// write the new file
			existing_entry->second = std::move(file);
		} else {
			table_delete_map.insert(make_pair(data_file_path, std::move(file)));
		}
	}
}

bool DuckLakeTransaction::HasLocalDeletes(TableIndex table_id) {
	return new_delete_files.find(table_id) != new_delete_files.end();
}

void DuckLakeTransaction::GetLocalDeleteForFile(TableIndex table_id, const string &path, string &result) {
	auto entry = new_delete_files.find(table_id);
	if (entry == new_delete_files.end()) {
		return;
	}
	auto file_entry = entry->second.find(path);
	if (file_entry == entry->second.end()) {
		return;
	}
	result = file_entry->second.file_name;
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
			for (auto &file : *table_entry->second) {
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

void DuckLakeTransaction::DropView(DuckLakeViewEntry &view) {
	if (view.IsTransactionLocal()) {
		// table is transaction-local - drop it from the transaction local changes
		auto schema_entry = new_tables.find(view.ParentSchema().name);
		if (schema_entry == new_tables.end()) {
			throw InternalException("Dropping a transaction local view that does not exist?");
		}
		schema_entry->second->DropEntry(view.name);
		new_tables.erase(schema_entry);
	} else {
		auto view_id = view.GetViewId();
		dropped_views.insert(view_id);
	}
}

void DuckLakeTransaction::DropEntry(CatalogEntry &entry) {
	switch (entry.type) {
	case CatalogType::TABLE_ENTRY:
		DropTable(entry.Cast<DuckLakeTableEntry>());
		break;
	case CatalogType::VIEW_ENTRY:
		DropView(entry.Cast<DuckLakeViewEntry>());
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
	case CatalogType::VIEW_ENTRY: {
		auto &view_entry = entry.Cast<DuckLakeViewEntry>();
		return dropped_views.find(view_entry.GetViewId()) != dropped_views.end();
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
	if (!new_entry) {
		return;
	}
	switch (entry.type) {
	case CatalogType::TABLE_ENTRY:
		AlterEntryInternal(entry.Cast<DuckLakeTableEntry>(), std::move(new_entry));
		break;
	case CatalogType::VIEW_ENTRY:
		AlterEntryInternal(entry.Cast<DuckLakeViewEntry>(), std::move(new_entry));
		break;
	default:
		throw NotImplementedException("Unsupported catalog type for AlterEntry");
	}
}

void DuckLakeTransaction::AlterEntryInternal(DuckLakeTableEntry &table, unique_ptr<CatalogEntry> new_entry) {
	auto &new_table = new_entry->Cast<DuckLakeTableEntry>();
	auto &entries = GetOrCreateTransactionLocalEntries(table);
	entries.CreateEntry(std::move(new_entry));
	switch (new_table.GetLocalChange().type) {
	case LocalChangeType::RENAMED: {
		// rename - take care of the old table
		if (table.IsTransactionLocal()) {
			// table is transaction local - delete the old table from there
			entries.DropEntry(table.name);
		} else {
			// table is not transaction local - add to drop list
			auto table_id = table.GetTableId();
			dropped_tables.insert(table_id);
		}
		break;
	}
	case LocalChangeType::ADD_COLUMN:
	case LocalChangeType::SET_PARTITION_KEY:
	case LocalChangeType::SET_COMMENT:
	case LocalChangeType::SET_COLUMN_COMMENT:
	case LocalChangeType::SET_NULL:
	case LocalChangeType::DROP_NULL:
	case LocalChangeType::RENAME_COLUMN:
	case LocalChangeType::REMOVE_COLUMN:
	case LocalChangeType::CHANGE_COLUMN_TYPE:
	case LocalChangeType::SET_DEFAULT:
		break;
	default:
		throw NotImplementedException("Alter type not supported in DuckLakeTransaction::AlterEntry");
	}
}

void DuckLakeTransaction::AlterEntryInternal(DuckLakeViewEntry &view, unique_ptr<CatalogEntry> new_entry) {
	auto &new_view = new_entry->Cast<DuckLakeViewEntry>();
	auto &entries = GetOrCreateTransactionLocalEntries(view);
	entries.CreateEntry(std::move(new_entry));
	switch (new_view.GetLocalChange().type) {
	case LocalChangeType::RENAMED: {
		// rename - take care of the old table
		if (view.IsTransactionLocal()) {
			// view is transaction local - delete the old table from there
			entries.DropEntry(view.name);
		} else {
			// view is not transaction local - add to drop list
			auto table_id = view.GetViewId();
			dropped_views.insert(table_id);
		}
		break;
	}
	case LocalChangeType::SET_COMMENT:
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
	switch (catalog_type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY: {
		auto new_table_list = make_uniq<DuckLakeCatalogSet>();
		auto &result = *new_table_list;
		new_tables.insert(make_pair(schema_name, std::move(new_table_list)));
		return result;
	}
	default:
		throw InternalException("Catalog type not supported for transaction local storage");
	}
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
	switch (catalog_type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY: {
		auto entry = new_tables.find(schema_name);
		if (entry == new_tables.end()) {
			return nullptr;
		}
		return entry->second;
	}
	default:
		return nullptr;
	}
}

} // namespace duckdb
