//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "storage/ducklake_catalog_set.hpp"
#include "storage/ducklake_stats.hpp"
#include "storage/ducklake_partition_data.hpp"

namespace duckdb {
class ColumnList;
class DuckLakeFieldData;

class DuckLakeCatalog : public Catalog {
public:
	DuckLakeCatalog(AttachedDatabase &db_p, string metadata_database, string metadata_path, string data_path,
	                string metadata_schema);
	~DuckLakeCatalog();

public:
	void Initialize(bool load_builtin) override;
	void Initialize(optional_ptr<ClientContext> context, bool load_builtin) override;
	string GetCatalogType() override {
		return "ducklake";
	}
	const string &MetadataDatabaseName() const {
		return metadata_database;
	}
	const string &MetadataSchemaName() const {
		return metadata_schema;
	}
	const string &MetadataPath() const {
		return metadata_path;
	}
	const string &DataPath() const {
		return data_path;
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	optional_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, TableIndex table_id);
	optional_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                               TableIndex table_id);

	bool InMemory() override;
	string GetDBPath() override;

	bool SupportsTimeTravel() const override {
		return true;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

	//! Return the schema for the given snapshot - loading it if it is not yet loaded
	DuckLakeCatalogSet &GetSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	unique_ptr<DuckLakeCatalogSet> LoadSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	PhysicalOperator &PlanCopyForInsert(ClientContext &context, const ColumnList &columns,
	                                    PhysicalPlanGenerator &planner, optional_ptr<DuckLakePartition> partition_data,
	                                    optional_ptr<DuckLakeFieldData> field_data,
	                                    optional_ptr<PhysicalOperator> plan);
	DuckLakeStats &GetStatsForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	unique_ptr<DuckLakeStats> LoadStatsForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                               DuckLakeCatalogSet &schema);

private:
	mutex schemas_lock;
	//! Map of schema index -> schema
	unordered_map<idx_t, unique_ptr<DuckLakeCatalogSet>> schemas;
	//! Map of data file index -> table stats
	unordered_map<idx_t, unique_ptr<DuckLakeStats>> stats;
	string metadata_database;
	string metadata_path;
	string data_path;
	string metadata_schema;
};

} // namespace duckdb
