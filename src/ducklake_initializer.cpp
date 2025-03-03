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
	// initialize the ducklake tables
	auto &table = CreateTable("ducklake_info", {"data_path"}, {LogicalType::VARCHAR});
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
