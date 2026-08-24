#include "ducklake_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "storage/ducklake_storage.hpp"
#include "storage/ducklake_scan.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_secret.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_log_type.hpp"
#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_secret.hpp"
#include "storage/ducklake_storage.hpp"

namespace duckdb {

ScalarFunction DuckLakeMurmur3Function();

#ifdef DUCKLAKE_KMS_PROVIDER
//! Hand control to the concrete KMS provider this build was overlaid with, so
//! it can register itself as the DuckLakeEncryptionProvider factory.
//!
//! WHY A CALL AND NOT A STATIC INITIALIZER. A provider's natural registration
//! path is a file-scope object whose constructor runs before main(). That works
//! in the LOADABLE extension, where every object file is linked into the shared
//! library. It does NOT work in the duckdb binary: there DuckLake is linked as
//! libducklake_extension.a, and an archive member that no symbol references is
//! never pulled out of the archive. The provider registered nothing, and every
//! ATTACH with ENCRYPTION_SOCKET was refused by a build that contained the
//! provider's source and none of its code. This declaration is the reference
//! that makes the link REQUIRE it.
//!
//! It names no KMS. DUCKLAKE_KMS_PROVIDER is defined by src/CMakeLists.txt only
//! when a provider directory is present, so a build without one neither
//! declares nor calls this, and nothing here depends on which KMS is behind it.
void DuckLakeRegisterKmsProvider();
#endif

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Adds support for DuckLake, SQL as a Lakehouse Format");

#ifdef DUCKLAKE_KMS_PROVIDER
	// Before anything can ATTACH, so the factory is in place by the time
	// DuckLakeCatalog's constructor asks for it.
	DuckLakeRegisterKmsProvider();
#endif

	auto &instance = loader.GetDatabaseInstance();
	instance.GetLogManager().RegisterLogType(make_uniq<DuckLakeMetadataLogType>());

	auto &config = DBConfig::GetConfig(instance);
	StorageExtension::Register(config, "ducklake", make_shared_ptr<DuckLakeStorageExtension>());

	config.AddExtensionOption("ducklake_max_retry_count",
	                          "The maximum amount of retry attempts for a ducklake transaction", LogicalType::UBIGINT,
	                          Value::UBIGINT(10), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_retry_wait_ms", "Time between retries", LogicalType::UBIGINT,
	                          Value::UBIGINT(100), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_retry_backoff", "Backoff factor for exponentially increasing retry wait time",
	                          LogicalType::DOUBLE, Value::DOUBLE(1.5), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_default_data_inlining_row_limit",
	                          "Default row limit for data inlining (0 disables inlining)", LogicalType::UBIGINT,
	                          Value::UBIGINT(10), nullptr, SetScope::GLOBAL);
	auto set_target_file_size = [](ClientContext &, SetScope, Value &parameter) {
		if (!parameter.IsNull() && !parameter.ToString().empty()) {
			DBConfig::ParseMemoryLimit(parameter.ToString());
		}
	};
	config.AddExtensionOption("ducklake_target_file_size", "Target file size for insertion and compaction",
	                          LogicalType::VARCHAR, Value(), set_target_file_size, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_write_deletion_vectors",
	                          "[EXPERIMENTAL] Write Iceberg V3 deletion vectors (puffin) instead of "
	                          "positional delete files (parquet)",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);

	DuckLakeSnapshotsFunction snapshots;
	loader.RegisterFunction(snapshots);

	DuckLakeTableInfoFunction table_info;
	loader.RegisterFunction(table_info);

	auto table_insertions = DuckLakeTableInsertionsFunction::GetFunctions();
	loader.RegisterFunction(table_insertions);

	auto table_deletions = DuckLakeTableDeletionsFunction::GetFunctions();
	loader.RegisterFunction(table_deletions);

	auto merge_adjacent_files = DuckLakeMergeAdjacentFilesFunction::GetFunctions();
	loader.RegisterFunction(merge_adjacent_files);

	auto rewrite_files = DuckLakeRewriteDataFilesFunction::GetFunctions();
	loader.RegisterFunction(rewrite_files);

	DuckLakeCleanupOldFilesFunction cleanup_old_files;
	loader.RegisterFunction(cleanup_old_files);

	DuckLakeCleanupOrphanedFilesFunction cleanup_orphaned_files;
	loader.RegisterFunction(cleanup_orphaned_files);

	DuckLakeExpireSnapshotsFunction expire_snapshots;
	loader.RegisterFunction(expire_snapshots);

	DuckLakeFlushInlinedDataFunction flush_inlined_data;
	loader.RegisterFunction(flush_inlined_data);

	DuckLakeSetOptionFunction set_options;
	loader.RegisterFunction(set_options);

	DuckLakeOptionsFunction options;
	loader.RegisterFunction(options);

	DuckLakeSetCommitMessage set_commit_message;
	loader.RegisterFunction(set_commit_message);

	auto table_changes = DuckLakeTableInsertionsFunction::GetDuckLakeTableChanges();
	loader.RegisterFunction(*table_changes);

	DuckLakeListFilesFunction list_files;
	loader.RegisterFunction(list_files);

	auto add_files = DuckLakeAddDataFilesFunction::GetFunctions();
	loader.RegisterFunction(add_files);

	DuckLakeCurrentSnapshotFunction current_snapshot;
	loader.RegisterFunction(current_snapshot);

	DuckLakeLastCommittedSnapshotFunction last_committed;
	loader.RegisterFunction(last_committed);

	DuckLakeSettingsFunction settings;
	loader.RegisterFunction(settings);

	DuckLakeCommitFunction commit;
	loader.RegisterFunction(commit);

	// Register ducklake_scan so it can be found during deserialization
	auto ducklake_scan = DuckLakeFunctions::GetDuckLakeScanFunction(loader.GetDatabaseInstance());
	loader.RegisterFunction(ducklake_scan);

	// secrets
	auto secret_type = DuckLakeSecret::GetSecretType();
	loader.RegisterSecretType(secret_type);

	auto ducklake_secret_function = DuckLakeSecret::GetFunction();
	loader.RegisterFunction(ducklake_secret_function);

	// Register murmur3_32 scalar function for Iceberg-compatible bucket
	// partitioning
	auto murmur3_func = DuckLakeMurmur3Function();
	loader.RegisterFunction(murmur3_func);

	// register rewrap_keys — the consumer half of a KMS key rotation
	auto rewrap_keys = DuckLakeRewrapKeysFunction();
	loader.RegisterFunction(rewrap_keys);

	// register ducklake_self_test — boot-time health check (M5)
	auto self_test = DuckLakeSelfTestFunction();
	loader.RegisterFunction(self_test);
}

void DucklakeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string DucklakeExtension::Name() {
	return "ducklake";
}

std::string DucklakeExtension::Version() const {
#ifdef EXT_VERSION_DUCKLAKE
	return EXT_VERSION_DUCKLAKE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ducklake, loader) {
	LoadInternal(loader);
}
}
