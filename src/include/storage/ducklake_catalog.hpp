//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ducklake_encryption.hpp"
// PRIVATE-FORK ONLY: crypta envelope encryption. Not upstream-eligible.
#include "crypta/ducklake_crypta.hpp"
#include "common/ducklake_options.hpp"
#include "common/ducklake_name_map.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "storage/ducklake_catalog_set.hpp"
#include "storage/ducklake_partition_data.hpp"
#include "storage/ducklake_stats.hpp"

#include <chrono>
#include <functional>

namespace duckdb {
struct DuckLakeGlobalStatsInfo;
class ColumnList;
class DuckLakeFieldData;
struct DuckLakeFileListEntry;
struct DuckLakeConfigOption;
struct DuckLakeSnapshotCommit;
struct DeleteFileMap;
class LogicalGet;

//! Per-table stats cache entry, keyed by <next_file_id, table_id>.
struct DuckLakeTableStatsCacheEntry : public ObjectCacheEntry {
	static constexpr idx_t ESTIMATED_BYTES_PER_COLUMN_STATS = 256;

	explicit DuckLakeTableStatsCacheEntry(DuckLakeTableStats stats_p) : stats(std::move(stats_p)) {
	}

	DuckLakeTableStats stats;

	static string ObjectType() {
		return "ducklake_table_stats";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override;
};

//! Cache entry for a DuckLake schema version
struct DuckLakeSchemaCacheEntry : public ObjectCacheEntry {
	explicit DuckLakeSchemaCacheEntry(unique_ptr<DuckLakeCatalogSet> catalog_set_p)
	    : catalog_set(std::move(*catalog_set_p)) {
	}

	DuckLakeCatalogSet catalog_set;

	static string ObjectType() {
		return "ducklake_schema";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override;
};

//! Query-scoped pin for DuckLake schema cache entries, which guarantee memory safety before transaction finishes.
class DuckLakeSchemaPinState : public ClientContextState {
public:
	void QueryEnd(ClientContext &context) override;
	void Pin(shared_ptr<DuckLakeSchemaCacheEntry> entry);

private:
	mutex lock;
	// Maps from address of the schema cache entry to the schema cache entry.
	unordered_map<DuckLakeSchemaCacheEntry *, shared_ptr<DuckLakeSchemaCacheEntry>> pins;
};

enum class InlinedDeletionCacheResult { EXISTS, DOES_NOT_EXIST, UNKNOWN };

class DuckLakeCatalog : public Catalog {
public:
	// default target file size: 512MB
	static constexpr const idx_t DEFAULT_TARGET_FILE_SIZE = 1 << 29;

public:
	DuckLakeCatalog(AttachedDatabase &db_p, DuckLakeOptions options);
	~DuckLakeCatalog() override;

public:
	void Initialize(bool load_builtin) override;
	void Initialize(optional_ptr<ClientContext> context, bool load_builtin) override;
	void FinalizeLoad(optional_ptr<ClientContext> context) override;
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
	const string &MetadataType() const {
		return metadata_type;
	}
	idx_t DataInliningRowLimit(SchemaIndex schema_index, TableIndex table_index) const;
	idx_t DataInliningRowLimit(ClientContext &context, SchemaIndex schema_index, TableIndex table_index) const;
	//! Returns the inlining limit (0 if the table is not eligible)
	idx_t GetInliningLimit(ClientContext &context, DuckLakeTableEntry &table);
	idx_t GetTargetFileSize(ClientContext &context, SchemaIndex schema_id, TableIndex table_id) const;
	idx_t GetTargetFileSize(ClientContext &context, DuckLakeTableEntry &table) const;
	string &Separator() {
		return separator;
	}
	void SetConfigOption(const DuckLakeConfigOption &option);
	bool TryGetConfigOption(const string &option, string &result, SchemaIndex schema_id, TableIndex table_id) const;
	//! Check if a config option has a table-level or schema-level override (excluding global scope)
	bool TryGetScopedConfigOption(const string &option, string &result, SchemaIndex schema_id,
	                              TableIndex table_id) const;
	template <class T>
	T GetConfigOption(const string &option, SchemaIndex schema_id, TableIndex table_id, T default_value) const {
		string value_str;
		if (TryGetConfigOption(option, value_str, schema_id, table_id)) {
			return Value(value_str).GetValue<T>();
		}
		return default_value;
	}
	bool TryGetConfigOption(const string &option, string &result, DuckLakeTableEntry &table) const;

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
	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;
	unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
	                                              unique_ptr<LogicalOperator> plan,
	                                              unique_ptr<CreateIndexInfo> create_info,
	                                              unique_ptr<AlterTableInfo> alter_info) override;
	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	shared_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, TableIndex table_id);
	shared_ptr<DuckLakeTableStats> GetTableStats(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                             TableIndex table_id);

	optional_ptr<CatalogEntry> GetEntryById(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                        SchemaIndex schema_id);
	optional_ptr<CatalogEntry> GetEntryById(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot,
	                                        TableIndex table_id);
	string GeneratePathFromName(const string &uuid, const string &name);

	bool InMemory() override;
	string GetDBPath() override;

	string GetDataPath();

	bool SupportsTimeTravel() const override {
		return true;
	}

	DuckLakeEncryption Encryption() const {
		return options.encryption;
	}

	bool IsEncrypted() const override {
		return Encryption() == DuckLakeEncryption::ENCRYPTED;
	}

	//! PRIVATE-FORK ONLY (crypta envelope encryption). Not upstream-eligible.
	//! The envelope provider for this lake, or nullptr when crypta_socket was not
	//! set - in which case the encryption_key column holds a plaintext key
	//! exactly as upstream, and nothing in the crypta path runs.
	optional_ptr<DuckLakeCryptaProvider> CryptaProvider() const {
		return crypta_provider.get();
	}
	//! Build a file identity for the crypta binding. `stored_path` must be the
	//! path AS PERSISTED in the catalog, not one resolved against the data path.
	CryptaFileIdentity CryptaIdentity(TableIndex table_id, const string &stored_path, bool is_delete_file) const;
	//! PRIVATE-FORK ONLY (crypta envelope encryption). Not upstream-eligible.
	//! Throw if `stored_key` is a crypta-wrapped blob while THIS lake has no
	//! crypta provider - i.e. the lake was attached without CRYPTA_SOCKET /
	//! CRYPTA_LAKE_ID. Call it immediately before any site that would otherwise
	//! base64-decode a stored key and use the bytes as a Parquet encryption key.
	//!
	//! It is a shared helper rather than an inlined check because there is more
	//! than one such site and they do NOT share a code path: ReadDataFile is the
	//! choke point for ordinary reads, and ducklake_flush_inlined_data.cpp
	//! queries ducklake_delete_file directly with its own SQL and never goes
	//! near it. That second site is exactly how the first version of this guard
	//! was incomplete. Any NEW decode site must call this too.
	//!
	//! Deliberately NOT an unwrap: it only refuses the unconfigured case. It is
	//! ONE THIRD of ResolveStoredEncryptionKey below, which is what every decode
	//! site should call - this stays a separate function only because the refusal
	//! is worth being able to mutate on its own.
	void RefuseWrappedKeyWithoutCrypta(const string &file_path, const string &stored_key) const;
	//! PRIVATE-FORK ONLY (crypta envelope encryption). Not upstream-eligible.
	//! Throw if THIS lake is ENCRYPTED and the stored `encryption_key` column is
	//! NULL - a file that must have a key and has none. On an unencrypted lake a
	//! NULL is the ordinary case and this returns.
	//!
	//! The message is upstream's, character for character, because it is what
	//! every existing test and every operator runbook greps for.
	//!
	//! Another third of ResolveStoredEncryptionKey, split out for the same reason
	//! as the refusal above: a guard nothing can remove on its own is a guard
	//! nothing can prove.
	void RefuseMissingEncryptionKey(const string &file_path) const;
	//! PRIVATE-FORK ONLY (crypta envelope encryption). Not upstream-eligible.
	//! THE key-resolution choke point. Turn a stored `encryption_key` column
	//! value into the raw key bytes the Parquet reader wants. It asks THREE
	//! questions, in this order, and a caller inherits all three or none:
	//!
	//!   1. the column is NULL   -> refuse if this lake is ENCRYPTED (the file
	//!                              must have a key and has none); otherwise
	//!                              there is no key, and that is correct.
	//!   2. crypta configured    -> unwrap it through the provider, which also
	//!                              verifies the identity binding and refuses a
	//!                              PLAINTEXT row (a downgrade attempt, or a
	//!                              leftover from before the envelope);
	//!   3. no crypta            -> refuse a WRAPPED blob (the operator dropped
	//!                              the options), otherwise base64-decode it
	//!                              exactly as upstream does.
	//!
	//! `stored_key` is the COLUMN VALUE, nullable, and not a `string` a caller has
	//! already decided is present. That is the whole point of the signature and it
	//! is not a convenience: while question 1 lived at the call sites, a site could
	//! - and one did - guard the call with its own `IsNull()` test and so inherit
	//! two questions of three. There is no `if` for a call site to get wrong now.
	//!
	//! `stored_path` MUST be the path as PERSISTED in the catalog - the identity
	//! crypta verifies is built from it. `resolved_path` is for the refusals'
	//! error messages only, so an operator is told which file on disk to look at.
	//!
	//! Every site that reads an `encryption_key` column calls THIS, not a part of
	//! it. Two sites read that column and they do NOT share a code path -
	//! ReadDataFile, and ducklake_flush_inlined_data.cpp, which queries
	//! ducklake_delete_file with its own SQL. Each having its own copy of the
	//! decision is how #26 happened: the flush site carried the refusal and never
	//! grew the unwrap, so a CONFIGURED lake handed a wrapped blob to mbedtls as a
	//! key. Extracting only PART of the decision is how #53 happened right after
	//! it: questions 2 and 3 moved here, question 1 stayed inline in ReadDataFile,
	//! and the flush site skipped the call entirely on a NULL - so an ENCRYPTED
	//! lake's missing delete-file key was refused by the scan path and accepted by
	//! the flush path. One function, all three questions, is what makes a third
	//! decode site inherit the whole decision instead of a part of it.
	string ResolveStoredEncryptionKey(TableIndex table_id, const string &stored_path, const string &resolved_path,
	                                  bool is_delete_file, const Value &stored_key) const;

	bool IsCommitInfoRequired() const {
		auto require = GetConfigOption<string>("require_commit_message", {}, {}, "false");
		return require == "true";
	}

	void EnsureCommitInfoProvided(const DuckLakeSnapshotCommit &commit_info) const;

	bool UseHiveFilePattern(bool default_value, SchemaIndex schema_id, TableIndex table_id) const {
		auto hive_file_pattern =
		    GetConfigOption<string>("hive_file_pattern", schema_id, table_id, default_value ? "true" : "false");
		return hive_file_pattern == "true";
	}

	bool WriteDeletionVectors(SchemaIndex schema_id, TableIndex table_id) const {
		auto write_dv = GetConfigOption<string>("write_deletion_vectors", schema_id, table_id, "false");
		return write_dv == "true";
	}

	void SetEncryption(DuckLakeEncryption encryption);
	// Generate an encryption key for writing (or empty if encryption is disabled)
	string GenerateEncryptionKey(ClientContext &context) const;

	void OnDetach(ClientContext &context) override;

	optional_idx GetCatalogVersion(ClientContext &context) override;

	idx_t GetNewUncommittedCatalogVersion() {
		return ++last_uncommitted_catalog_version;
	}

	void SetCommittedSnapshotId(idx_t value) {
		lock_guard<mutex> guard(commit_lock);
		last_committed_snapshot = value;
	}

	//! Whether the metadata server can execute the commit retry loop server-side.
	bool RetrialsServerSide() const {
		return retrials_server_side;
	}
	void SetRetrialsServerSide(bool value) {
		retrials_server_side = value;
	}

	Value GetLastCommittedSnapshotId() const {
		lock_guard<mutex> guard(commit_lock);
		if (last_committed_snapshot.IsValid()) {
			return Value::UBIGINT(last_committed_snapshot.GetIndex());
		}
		return Value();
	}

	optional_ptr<const DuckLakeNameMap> TryGetMappingById(DuckLakeTransaction &transaction, MappingIndex mapping_id);
	MappingIndex TryGetCompatibleNameMap(DuckLakeTransaction &transaction, const DuckLakeNameMap &name_map);
	idx_t GetBeginSnapshotForTable(TableIndex table_id, DuckLakeTransaction &transaction);
	idx_t GetBeginSnapshotForSchemaVersion(TableIndex table_id, idx_t schema_version, DuckLakeTransaction &transaction);

	static unique_ptr<DuckLakeStats> ConstructStatsMap(vector<DuckLakeGlobalStatsInfo> &global_stats,
	                                                   DuckLakeCatalogSet &schema);
	//! Return the schema for the given snapshot - loading it if it is not yet loaded
	DuckLakeCatalogSet &GetSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);

	//! Callback type for instrumenting metadata queries
	using QueryCallback = std::function<void(const string &query, std::chrono::steady_clock::duration elapsed)>;

	void SetQueryCallback(QueryCallback callback) {
		query_callback = std::move(callback);
	}
	const QueryCallback &GetQueryCallback() const {
		return query_callback;
	}

	//! Check if an inlined deletion table is known to exist or not exist for the given table and snapshot
	InlinedDeletionCacheResult CheckInlinedDeletionTableCache(TableIndex table_id, DuckLakeSnapshot snapshot);
	//! Cache the result of an inlined deletion table existence check
	void CacheInlinedDeletionTableResult(TableIndex table_id, DuckLakeSnapshot snapshot, bool exists);

	//! Invalidate the cached table stats entry for a given stats cache key.
	void InvalidateTableStatsCache(idx_t next_file_id, TableIndex table_id);
	//! Invalidate the cached schema entry for a given schema_version.
	void InvalidateSchemaCache(idx_t schema_version);

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;
	unique_ptr<DuckLakeCatalogSet> LoadSchemaForSnapshot(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot);
	//! Look up (or load) the ObjectCache entry for a given snapshot.
	shared_ptr<DuckLakeSchemaCacheEntry> GetSchemaCacheEntry(DuckLakeTransaction &transaction,
	                                                         DuckLakeSnapshot snapshot);
	//! Pin a schema cache entry for the duration of the current query to ensure safe memory access.
	void PinSchemaForQuery(DuckLakeTransaction &transaction, shared_ptr<DuckLakeSchemaCacheEntry> entry);
	void LoadNameMaps(DuckLakeTransaction &transaction);
	string StatsCacheKey(idx_t next_file_id, TableIndex table_id) const;
	string SchemaCacheKey(idx_t schema_version) const;
	string SchemaPinStateKey() const;
	ObjectCache &GetObjectCacheInstance();

private:
	mutex name_maps_lock;
	//! Map of mapping index -> name map
	DuckLakeNameMapSet name_maps;
	//! The maximum name map index we have loaded so far
	optional_idx loaded_name_map_index;
	//! The configuration lock
	mutable mutex config_lock;
	//! The DuckLake options
	DuckLakeOptions options;
	//! PRIVATE-FORK ONLY: envelope key provider, null unless crypta_socket is set.
	unique_ptr<DuckLakeCryptaProvider> crypta_provider;
	//! The path separator
	string separator = "/";
	//! A unique tracker for catalog changes in uncommitted transactions.
	atomic<idx_t> last_uncommitted_catalog_version;
	//! The metadata server type
	string metadata_type;
	//! A per-instance identifier used to scope ObjectCache keys.
	string instance_id;
	//! Whether or not the catalog is initialized
	bool initialized = false;
	//! Whether or not the metadata server can execute the commit retry loop server-side.
	bool retrials_server_side = false;
	//! Cache for inlined deletion table existence checks
	mutex inlined_deletion_cache_lock;
	//! Table IDs where the inlined deletion table is known to exist (permanent - never invalidated)
	unordered_set<idx_t> inlined_deletion_exists;
	//! Table IDs where the inlined deletion table is known to NOT exist, with the snapshot_id at which we checked
	//! Valid as long as current snapshot.snapshot_id <= cached snapshot_id
	unordered_map<idx_t, idx_t> inlined_deletion_not_exists;
	//! The id of the last committed snapshot, set at FlushChanges on a successful commit
	mutable mutex commit_lock;
	optional_idx last_committed_snapshot;
	//! Optional callback for instrumenting metadata queries
	QueryCallback query_callback;
};

} // namespace duckdb
