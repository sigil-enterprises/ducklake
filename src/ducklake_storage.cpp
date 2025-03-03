#include "duckdb.hpp"

#include "ducklake_storage.hpp"
#include "ducklake_catalog.hpp"
#include "ducklake_transaction_manager.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog> DuckLakeAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AccessMode access_mode) {
	auto &db_manager = DatabaseManager::Get(db);

	// attach the child database
	auto metadata_attach_name = "__internal_ducklake_metadata";
	auto attach_info = make_uniq<AttachInfo>();
	attach_info->name = metadata_attach_name;
	attach_info->path = info.path;
	AttachOptions attach_options(attach_info, access_mode);
	optional_ptr<AttachedDatabase> metadata_database;
	try {
		metadata_database = db_manager.AttachDatabase(context, *attach_info, attach_options);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		error.Throw("Failed to attach DuckLake \"" + name + "\" at path + \"" + info.path + "\"");
	}
	return make_uniq<DuckLakeCatalog>(db, std::move(metadata_attach_name));
}

static unique_ptr<TransactionManager> DuckLakeCreateTransactionManager(StorageExtensionInfo *storage_info,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	return make_uniq<DuckLakeTransactionManager>(db, ducklake_catalog);
}

DuckLakeStorageExtension::DuckLakeStorageExtension() {
	attach = DuckLakeAttach;
	create_transaction_manager = DuckLakeCreateTransactionManager;
}

} // namespace duckdb
