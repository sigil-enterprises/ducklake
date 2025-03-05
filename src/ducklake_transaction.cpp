#include "ducklake_transaction.hpp"
#include "duckdb/main/database_manager.hpp"
#include "ducklake_catalog.hpp"
#include "ducklake_schema_entry.hpp"
#include "ducklake_table_entry.hpp"

namespace duckdb {

DuckLakeTransaction::DuckLakeTransaction(DuckLakeCatalog &ducklake_catalog, TransactionManager &manager,
                                         ClientContext &context)
    : Transaction(manager, context), ducklake_catalog(ducklake_catalog), db(*context.db) {
}

DuckLakeTransaction::~DuckLakeTransaction() {
}

void DuckLakeTransaction::Start() {
}

void DuckLakeTransaction::Commit() {
	if (connection) {
		FlushChanges();
		connection->Commit();
		connection.reset();
	}
}

void DuckLakeTransaction::Rollback() {
	if (connection) {
		connection->Rollback();
		connection.reset();
	}
}

Connection &DuckLakeTransaction::GetConnection() {
	if (!connection) {
		connection = make_uniq<Connection>(db);
		connection->BeginTransaction();
	}
	return *connection;
}

bool DuckLakeTransaction::ChangesMade() {
	return !new_tables.empty() || !new_data_files.empty();
}

void DuckLakeTransaction::FlushChanges() {
	if (!ChangesMade()) {
		// read-only transactions don't need to do anything
		return;
	}
	auto commit_snapshot = GetSnapshot();

	commit_snapshot.snapshot_id++;
	auto changed_schema = !new_tables.empty();
	if (changed_schema) {
		// we changed the schema - need to get a new schema version
		commit_snapshot.schema_version++;
	}
	// write the new snapshot
	auto result =
	    Query(commit_snapshot,
	          R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES ({SNAPSHOT_ID}, NOW(), {SCHEMA_VERSION});)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to write new snapshot to DuckLake:");
	}
	// write new tables
	for (auto &schema_entry : new_tables) {
		for (auto &entry : schema_entry.second->GetEntries()) {
			// CREATE TABLE ducklake_table(table_id BIGINT PRIMARY KEY, table_uuid UUID, begin_snapshot BIGINT,
			// end_snapshot BIGINT, schema_id BIGINT, table_name VARCHAR);
			// write any new tables that we created
			auto &table = entry.second->Cast<DuckLakeTableEntry>();
			auto &schema = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
			auto table_id = table.GetTableId();
			auto table_insert_query = StringUtil::Format(
			    R"(INSERT INTO {METADATA_CATALOG}.ducklake_table VALUES (%d, '%s', {SNAPSHOT_ID}, NULL, %d, '%s');)",
			    table_id, table.GetTableUUID(), schema.GetSchemaId(), table.name);
			result = Query(commit_snapshot, table_insert_query);
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to write new table to DuckLake:");
			}
			// write the columns
			// CREATE TABLE ducklake_column(column_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, table_id
			// BIGINT, column_order BIGINT, column_name VARCHAR, column_type VARCHAR, default_value VARCHAR);
			string column_insert_query;
			idx_t column_id = 0;
			for (auto &col : table.GetColumns().Logical()) {
				if (!column_insert_query.empty()) {
					column_insert_query += ", ";
				}
				column_insert_query +=
				    StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, %d, '%s', '%s', NULL)", column_id, table_id,
				                       column_id, col.GetName(), col.GetType().ToString());
			}
			column_insert_query = "INSERT INTO {METADATA_CATALOG}.ducklake_column VALUES " + column_insert_query;
			result = Query(commit_snapshot, column_insert_query);
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to write column information to DuckLake:");
			}
		}
	}
	// write new data files
	if (!new_data_files.empty()) {
		// FIXME: actually start at the correct data file number
		idx_t data_file_id = 0;
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
				    StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, NULL, '%s', 'parquet', NULL, NULL, NULL)",
				                       data_file_id, table_id, file);
				data_file_id++;
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
	return Query(std::move(query));
}

DuckLakeSnapshot DuckLakeTransaction::GetSnapshot() {
	if (!snapshot) {
		// no snapshot loaded yet for this transaction
		// query the snapshot id/schema version
		auto result = Query(
		    R"(SELECT snapshot_id, schema_version FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id = (SELECT MAX(snapshot_id) FROM {METADATA_CATALOG}.ducklake_snapshot);)");
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to query most recent snapshot for DuckLake: ");
		}
		auto chunk = result->Fetch();
		if (chunk->size() != 1) {
			throw InvalidInputException("Corrupt DuckLake - multiple snapshots returned from database");
		}

		auto snapshot_id = chunk->GetValue(0, 0).GetValue<idx_t>();
		auto schema_version = chunk->GetValue(1, 0).GetValue<idx_t>();
		snapshot = make_uniq<DuckLakeSnapshot>(snapshot_id, schema_version);
	}
	return *snapshot;
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

DuckLakeCatalogSet &DuckLakeTransaction::GetOrCreateNewTableElements(const string &schema_name) {
	auto entry = GetNewTableElements(schema_name);
	if (entry) {
		return *entry;
	}
	// need to create it
	auto new_table_list = make_uniq<DuckLakeCatalogSet>(CatalogType::TABLE_ENTRY, schema_name);
	auto &result = *new_table_list;
	new_tables.insert(make_pair(schema_name, std::move(new_table_list)));
	return result;
}

optional_ptr<DuckLakeCatalogSet> DuckLakeTransaction::GetNewTableElements(const string &schema_name) {
	auto entry = new_tables.find(schema_name);
	if (entry == new_tables.end()) {
		return nullptr;
	}
	return entry->second;
}

} // namespace duckdb
