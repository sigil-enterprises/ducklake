#include "ducklake_initializer.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "ducklake_schema_entry.hpp"
#include "duckdb/main/connection.hpp"
#include "ducklake_catalog.hpp"
#include "ducklake_transaction.hpp"

namespace duckdb {

DuckLakeInitializer::DuckLakeInitializer(ClientContext &context, DuckLakeCatalog &catalog,
                                         const string &metadata_database, const string &metadata_path, string &schema,
                                         string &data_path)
    : context(context), catalog(catalog), metadata_database(metadata_database), metadata_path(metadata_path),
      schema(schema), data_path(data_path) {
}

void DuckLakeInitializer::Initialize() {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	// attach the metadata database
	auto result = transaction.Query("ATTACH {METADATA_PATH} AS {METADATA_CATALOG_NAME_IDENTIFIER}");
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		error_obj.Throw("Failed to attach DuckLake MetaData \"" + metadata_database + "\" at path + \"" +
		                metadata_path + "\"");
	}
	bool has_explicit_schema = !schema.empty();
	if (schema.empty()) {
		// if the schema is not explicitly set by the user - set it to the default schema in the catalog
		schema = transaction.GetDefaultSchemaName();
	}
	// after the metadata database is attached initialize the ducklake
	// check if we are loading an existing DuckLake or creating a new one
	// FIXME: verify that all tables are in the correct format instead
	result = transaction.Query(
	    "SELECT COUNT(*) FROM duckdb_tables() WHERE database_name={METADATA_CATALOG_NAME_LITERAL} AND "
	    "schema_name={METADATA_SCHEMA_NAME_LITERAL} AND table_name LIKE 'ducklake_%'");
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		error_obj.Throw("Failed to load DuckLake table data");
	}
	auto count = result->Fetch()->GetValue(0, 0).GetValue<idx_t>();
	if (count == 0) {
		InitializeNewDuckLake(transaction, has_explicit_schema);
	} else {
		LoadExistingDuckLake(transaction);
	}
}

void DuckLakeInitializer::InitializeNewDuckLake(DuckLakeTransaction &transaction, bool has_explicit_schema) {
	if (data_path.empty()) {
		throw InvalidInputException("Attempting to create a new ducklake instance but data_path is not set - set the "
		                            "DATA_PATH parameter to the desired location of the data files");
	}
	string initialize_query;
	if (has_explicit_schema) {
		// if the schema is user provided create it
		initialize_query += "CREATE SCHEMA IF NOT EXISTS {METADATA_CATALOG};\n";
	}
	initialize_query += R"(
CREATE TABLE {METADATA_CATALOG}.ducklake_info(data_path VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_snapshot(snapshot_id BIGINT PRIMARY KEY, snapshot_time TIMESTAMPTZ, schema_version BIGINT, next_catalog_id BIGINT, next_file_id BIGINT);
CREATE TABLE {METADATA_CATALOG}.ducklake_snapshot_changes(snapshot_id BIGINT PRIMARY KEY, schemas_created VARCHAR, schemas_dropped VARCHAR, tables_created VARCHAR, tables_dropped VARCHAR, tables_altered VARCHAR, tables_inserted_into VARCHAR, tables_deleted_from VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_schema(schema_id BIGINT PRIMARY KEY, schema_uuid UUID, begin_snapshot BIGINT, end_snapshot BIGINT, schema_name VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_table(table_id BIGINT, table_uuid UUID, begin_snapshot BIGINT, end_snapshot BIGINT, schema_id BIGINT, table_name VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_table_statistics(table_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, record_count BIGINT);
CREATE TABLE {METADATA_CATALOG}.ducklake_table_column_statistics(column_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, table_id BIGINT, null_count BIGINT, lower_bound VARCHAR, upper_bound VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_column(column_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, table_id BIGINT, column_order BIGINT, column_name VARCHAR, column_type VARCHAR, default_value VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_data_file(data_file_id BIGINT PRIMARY KEY, begin_snapshot BIGINT, end_snapshot BIGINT, table_id BIGINT, file_order BIGINT, path VARCHAR, file_format VARCHAR, record_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, partition_id BIGINT);
CREATE TABLE {METADATA_CATALOG}.ducklake_file_column_statistics(data_file_id BIGINT, column_id BIGINT, column_size_bytes BIGINT, value_count BIGINT, null_count BIGINT, nan_count BIGINT, min_value VARCHAR, max_value VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_delete_file(delete_fle_id BIGINT PRIMARY KEY, begin_snapshot BIGINT, end_snapshot BIGINT, data_file_id BIGINT, path VARCHAR, delete_count BIGINT, file_size_bytes BIGINT);
INSERT INTO {METADATA_CATALOG}.ducklake_info VALUES ({DATA_PATH});
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES (0, NOW(), 0, 1, 0);
INSERT INTO {METADATA_CATALOG}.ducklake_schema VALUES (0, UUID(), 0, NULL, 'main');
	)";
	// TODO: add
	//	ducklake_partition_info
	//	ducklake_partition_column_info
	//	ducklake_partition_column_transforms
	//	ducklake_column_statistics
	//	ducklake_sorting_info
	//	ducklake_sorting_column_info
	//	ducklake_view
	//	ducklake_macro
	// TODO support schema parameter
	auto result = transaction.Query(initialize_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to initialize DuckLake:");
	}
}

void DuckLakeInitializer::LoadExistingDuckLake(DuckLakeTransaction &transaction) {
	// load the data path from the existing duck lake
	auto result = transaction.Query("SELECT data_path FROM {METADATA_CATALOG}.ducklake_info");
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		error_obj.Throw("Failed to load existing DuckLake");
	}
	auto chunk = result->Fetch();
	if (chunk->size() != 1) {
		throw InvalidInputException("Failed to load existing ducklake - ducklake_info does not have a single row");
	}
	if (data_path.empty()) {
		data_path = chunk->GetValue(0, 0).GetValue<string>();
	}
}

} // namespace duckdb
