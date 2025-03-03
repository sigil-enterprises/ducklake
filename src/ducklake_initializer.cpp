#include "ducklake_initializer.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

DuckLakeInitializer::DuckLakeInitializer(ClientContext &context, AttachedDatabase &metadata_database, const string &schema, const string &data_path) :
	context(context), metadata_database(metadata_database), schema(schema), data_path(data_path) {}


void DuckLakeInitializer::Initialize() {
	auto &catalog = metadata_database.GetCatalog();
	// check if the tables exist
	auto ducklake_info = catalog.GetEntry<TableCatalogEntry>(context, schema, "ducklake_info", OnEntryNotFound::RETURN_NULL);
	if (!ducklake_info) {
		auto &meta = MetaTransaction::Get(context);
		meta.ModifyDatabase(metadata_database);
		InitializeNewDuckLake();
	} else {
		LoadExistingDuckLake();
	}
}

void DuckLakeInitializer::InitializeNewDuckLake() {
	if (data_path.empty()) {
		throw InvalidInputException("Attempting to create a new ducklake instance but data_path is not set - set the DATA_PATH parameter to the desired location of the data files");
	}
	// FIXME: create schema if not exists/check schema
	// FIXME: if any of the tables exist - fail
	// initialize the ducklake tables
	auto &table = CreateTable("ducklake_info", {"data_path"}, {LogicalType::VARCHAR});
	CreateTable("ducklake_snapshot", {"snapshot_id", "snapshot_time"}, {LogicalType::BIGINT, LogicalType::TIMESTAMP});
	CreateTable("ducklake_schema", {"schema_id", "schema_uuid", "begin_snapshot", "end_snapshot", "schema_name"}, {LogicalType::BIGINT, LogicalType::UUID, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR});
	CreateTable("ducklake_table", {"table_id", "table_uuid", "begin_snapshot", "end_snapshot", "schema_id", "table_name"}, {LogicalType::BIGINT, LogicalType::UUID, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR});
	CreateTable("ducklake_column", {"column_id", "begin_snapshot", "end_snapshot", "table_id", "column_order", "column_name", "column_type", "default_value"}, {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR});
	CreateTable("ducklake_data_file", {"data_file_id", "begin_snapshot", "end_snapshot", "table_id", "file_order", "path", "file_format", "record_count", "file_size_bytes", "partition_id"}, {
	LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT});
	CreateTable("ducklake_delete_file", {"delete_file_id", "begin_snapshot", "end_snapshot", "data_file_id", "path", "delete_count", "file_size_bytes"}, {
	LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT});
//	CreateTable("ducklake_partition_info", {}, {});
//	CreateTable("ducklake_partition_column_info", {}, {});
//	CreateTable("ducklake_partition_column_transforms", {}, {});
//	CreateTable("ducklake_column_statistics", {}, {});
//	CreateTable("ducklake_sorting_info", {}, {});
//	CreateTable("ducklake_sorting_column_info", {}, {});
//	CreateTable("ducklake_view", {}, {});
//	CreateTable("ducklake_macro", {}, {});


	DataChunk data_path_chunk;
	vector<LogicalType> data_path_types = {LogicalType::VARCHAR};
	data_path_chunk.Initialize(context, data_path_types);
	data_path_chunk.SetValue(0, 0, Value(data_path));
	data_path_chunk.SetCardinality(1);

	Insert(table, data_path_chunk);
}

void DuckLakeInitializer::LoadExistingDuckLake() {

}

TableCatalogEntry &DuckLakeInitializer::CreateTable(string name, vector<string> column_names, vector<LogicalType> column_types) {
	auto &catalog = metadata_database.GetCatalog();
	auto table_info = make_uniq<CreateTableInfo>(metadata_database.GetName(), schema, std::move(name));

	for(idx_t i = 0; i < column_names.size(); i++) {
		ColumnDefinition column(std::move(column_names[i]), std::move(column_types[i]));
		table_info->columns.AddColumn(std::move(column));
	}
	return catalog.CreateTable(context, std::move(table_info))->Cast<TableCatalogEntry>();
}

void DuckLakeInitializer::Insert(TableCatalogEntry &table_entry, DataChunk &data) {
	// FIXME - we should have some catalog method for this
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table_entry);
	vector<column_t> column_ids;
	for(idx_t i = 0; i < data.ColumnCount(); i++) {
		column_ids.push_back(i);
	}
	table_entry.GetStorage().LocalWALAppend(table_entry, context, data, bound_constraints);
}
}
