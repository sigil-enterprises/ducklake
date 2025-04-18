#define DUCKDB_EXTENSION_MAIN

#include "ducklake_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "storage/ducklake_storage.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "duckdb/main/extension_util.hpp"

namespace duckdb {

static void LoadInternal(DatabaseInstance &instance) {
	auto &config = DBConfig::GetConfig(instance);
	config.storage_extensions["ducklake"] = make_uniq<DuckLakeStorageExtension>();

	DuckLakeSnapshotsFunction snapshots;
	ExtensionUtil::RegisterFunction(instance, snapshots);

	DuckLakeTableInsertionsFunction table_insertions;
	ExtensionUtil::RegisterFunction(instance, table_insertions);

	DuckLakeTableDeletionsFunction table_deletions;
	ExtensionUtil::RegisterFunction(instance, table_deletions);

	DuckLakeMergeAdjacentFilesFunction merge_adjacent_files;
	ExtensionUtil::RegisterFunction(instance, merge_adjacent_files);

	auto table_changes = DuckLakeTableInsertionsFunction::GetDuckLakeTableChanges();
	ExtensionUtil::RegisterFunction(instance, *table_changes);
}

void DucklakeExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
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

DUCKDB_EXTENSION_API void ducklake_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::DucklakeExtension>();
}

DUCKDB_EXTENSION_API const char *ducklake_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
