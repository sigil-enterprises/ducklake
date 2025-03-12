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
	static LogicalType ParseDuckLakeType(const string &type);

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<SchemaCatalogEntry> GetSchema(CatalogTransaction transaction, const string &schema_name,
	                                           OnEntryNotFound if_not_found,
	                                           QueryErrorContext error_context = QueryErrorContext()) override;

	unique_ptr<PhysicalOperator> PlanInsert(ClientContext &context, LogicalInsert &op,
	                                        unique_ptr<PhysicalOperator> plan) override;
	unique_ptr<PhysicalOperator> PlanCreateTableAs(ClientContext &context, LogicalCreateTable &op,
	                                               unique_ptr<PhysicalOperator> plan) override;
	unique_ptr<PhysicalOperator> PlanDelete(ClientContext &context, LogicalDelete &op,
	                                        unique_ptr<PhysicalOperator> plan) override;
	unique_ptr<PhysicalOperator> PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
	                                        LogicalDelete &op) override;
	unique_ptr<PhysicalOperator> PlanUpdate(ClientContext &context, LogicalUpdate &op,
	                                        unique_ptr<PhysicalOperator> plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	optional_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, idx_t table_id);
	optional_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                               idx_t table_id);

	bool InMemory() override;
	string GetDBPath() override;

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

	//! Return the schema for the given snapshot - loading it if it is not yet loaded
	DuckLakeCatalogSet &GetSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	unique_ptr<DuckLakeCatalogSet> LoadSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	unique_ptr<PhysicalOperator> PlanCopyForInsert(ClientContext &context, const ColumnList &columns,
	                                               optional_ptr<DuckLakePartition> partition_data,
	                                               unique_ptr<PhysicalOperator> plan);
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
