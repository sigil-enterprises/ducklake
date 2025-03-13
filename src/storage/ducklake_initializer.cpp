#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/connection.hpp"

#include "storage/ducklake_initializer.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_schema_entry.hpp"

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
	auto &metadata_manager = transaction.GetMetadataManager();
	metadata_manager.InitializeDuckLake(has_explicit_schema);
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
