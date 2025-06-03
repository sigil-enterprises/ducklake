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
#include "common/ducklake_encryption.hpp"
#include "common/ducklake_options.hpp"

namespace duckdb {
class ColumnList;
class DuckLakeFieldData;
struct DuckLakeFileListEntry;
struct DeleteFileMap;
class LogicalGet;

class DuckLakeCatalog : public Catalog {
public:
	DuckLakeCatalog(AttachedDatabase &db_p, DuckLakeOptions options);
	~DuckLakeCatalog();

public:
	void Initialize(bool load_builtin) override;
	void Initialize(optional_ptr<ClientContext> context, bool load_builtin) override;
	string GetCatalogType() override {
		return "ducklake";
	}
	const string &MetadataDatabaseName() const {
		return options.metadata_database;
	}
	const string &MetadataSchemaName() const {
		return options.metadata_schema;
	}
	const string &MetadataPath() const {
		return options.metadata_path;
	}
	const string &DataPath() const {
		return options.data_path;
	}
	idx_t DataInliningRowLimit() const {
		return options.data_inlining_row_limit;
	}
	string &Separator() {
		return separator;
	}
	void SetConfigOption(string option, string value);
	bool TryGetConfigOption(const string &option, string &result) const;

	optional_ptr<BoundAtClause> CatalogSnapshot() const;

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
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	optional_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, TableIndex table_id);
	optional_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                               TableIndex table_id);

	optional_ptr<CatalogEntry> GetEntryById(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                        SchemaIndex schema_id);
	optional_ptr<CatalogEntry> GetEntryById(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                        TableIndex table_id);
	string GeneratePathFromName(const string &uuid, const string &name);

	bool InMemory() override;
	string GetDBPath() override;

	bool SupportsTimeTravel() const override {
		return true;
	}

	DuckLakeEncryption Encryption() const {
		return options.encryption;
	}
	void SetEncryption(DuckLakeEncryption encryption);
	// Generate an encryption key for writing (or empty if encryption is disabled)
	string GenerateEncryptionKey(ClientContext &context) const;

	void OnDetach(ClientContext &context) override;

	optional_idx GetCatalogVersion(ClientContext &context) override;

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

	//! Return the schema for the given snapshot - loading it if it is not yet loaded
	DuckLakeCatalogSet &GetSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	unique_ptr<DuckLakeCatalogSet> LoadSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	DuckLakeStats &GetStatsForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	unique_ptr<DuckLakeStats> LoadStatsForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                               DuckLakeCatalogSet &schema);

private:
	mutex schemas_lock;
	//! Map of schema index -> schema
	unordered_map<idx_t, unique_ptr<DuckLakeCatalogSet>> schemas;
	//! Map of data file index -> table stats
	unordered_map<idx_t, unique_ptr<DuckLakeStats>> stats;
	//! The configuration lock
	mutable mutex config_lock;
	//! The DuckLake options
	DuckLakeOptions options;
	// The path separator
	string separator = "/";
};

} // namespace duckdb
