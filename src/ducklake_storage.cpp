#include "duckdb.hpp"

#include "ducklake_storage.hpp"
#include "ducklake_catalog.hpp"
#include "ducklake_transaction_manager.hpp"
#include "duckdb/main/database_manager.hpp"
#include "ducklake_initializer.hpp"

namespace duckdb {

static unique_ptr<Catalog> DuckLakeAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AccessMode access_mode) {
	auto &db_manager = DatabaseManager::Get(db);

	// attach the child database
	optional_ptr<AttachedDatabase> metadata_database;
	string schema;
	string data_path;
	string metadata_catalog_name;
	for (auto &entry : info.options) {
		if (StringUtil::CIEquals(entry.first, "data_path")) {
			data_path = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "schema")) {
			schema = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "metadata_catalog")) {
			metadata_catalog_name = entry.second.ToString();
		}
	}
	if (metadata_catalog_name.empty()) {
		metadata_catalog_name = "__ducklake_metadata_" + name;
	}
	auto attach_info = make_uniq<AttachInfo>();
	attach_info->name = metadata_catalog_name;
	attach_info->path = info.path;
	AttachOptions attach_options(attach_info, access_mode);
	try {
		metadata_database = db_manager.AttachDatabase(context, *attach_info, attach_options);
		metadata_database->Initialize();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		error.Throw("Failed to attach DuckLake \"" + name + "\" at path + \"" + info.path + "\"");
	}
	// initialize the metadata database
	DuckLakeInitializer initializer(context, *metadata_database, schema, data_path);
	initializer.Initialize();

	return make_uniq<DuckLakeCatalog>(db, std::move(metadata_catalog_name), info.path);
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
