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
                                         const string &metadata_database,
	                    const string &metadata_path, const string &schema, const string &data_path)
    : context(context), catalog(catalog), metadata_database(metadata_database), metadata_path(metadata_path), schema(schema), data_path(data_path) {
}

void DuckLakeInitializer::Initialize() {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	// attach the metadata database
	auto result = transaction.Query("ATTACH '{METADATA_PATH}' AS \"{METADATA_CATALOG}\"");
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		error_obj.Throw("Failed to attach DuckLake MetaData \"" + metadata_database + "\" at path + \"" + metadata_path + "\"");
	}
	// FIXME: query duckdb_tables() instead?
	// after the metadata database is attached initialize the ducklake
	result = transaction.Query("SELECT * FROM {METADATA_CATALOG}.ducklake_info");
	if (result->HasError()) {
		InitializeNewDuckLake(transaction);
	} else {
		LoadExistingDuckLake(transaction);
	}
}

void DuckLakeInitializer::InitializeNewDuckLake(DuckLakeTransaction &transaction) {
	if (data_path.empty()) {
		throw InvalidInputException("Attempting to create a new ducklake instance but data_path is not set - set the "
		                            "DATA_PATH parameter to the desired location of the data files");
	}
	string initialize_query = R"(
	CREATE TABLE {METADATA_CATALOG}.ducklake_info(data_path VARCHAR);
	CREATE TABLE {METADATA_CATALOG}.ducklake_snapshot(snapshot_id BIGINT, snapshot_time TIMESTAMPTZ, schema_version BIGINT);
	CREATE TABLE {METADATA_CATALOG}.ducklake_schema(schema_id BIGINT, schema_uuid UUID, begin_snapshot BIGINT, end_snapshot BIGINT, schema_name VARCHAR);
	CREATE TABLE {METADATA_CATALOG}.ducklake_table(table_id BIGINT, table_uuid UUID, begin_snapshot BIGINT, end_snapshot BIGINT, table_name VARCHAR);
	CREATE TABLE {METADATA_CATALOG}.ducklake_column(column_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, table_id BIGINT, column_order BIGINT, column_name VARCHAR, column_type VARCHAR, default_value VARCHAR);
	CREATE TABLE {METADATA_CATALOG}.ducklake_data_file(data_file_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, table_id BIGINT, file_order BIGINT, path VARCHAR, file_format VARCHAR, record_count BIGINT, file_size_bytes BIGINT, partition_id BIGINT);
	CREATE TABLE {METADATA_CATALOG}.ducklake_delete_file(delete_fle_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT, data_file_id BIGINT, path VARCHAR, delete_count BIGINT, file_size_bytes BIGINT);
	INSERT INTO {METADATA_CATALOG}.ducklake_info VALUES ('{DATA_PATH}');
	INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES (0, NOW(), 0);
	INSERT INTO {METADATA_CATALOG}.ducklake_schema VALUES (0, uuid(), 0, NULL, 'main');
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
	throw InternalException("FIXME: load existing duck lake");
}

} // namespace duckdb
