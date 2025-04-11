#include "duckdb.hpp"

#include "storage/ducklake_storage.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog> DuckLakeAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AccessMode access_mode) {
	string schema;
	string data_path;
	string metadata_catalog_name;
	auto encrypted = DuckLakeEncryption::AUTOMATIC;
	for (auto &entry : info.options) {
		if (StringUtil::CIEquals(entry.first, "data_path")) {
			data_path = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "metadata_schema")) {
			schema = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "metadata_catalog")) {
			metadata_catalog_name = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "encrypted")) {
			if (entry.second.GetValue<bool>()) {
				encrypted = DuckLakeEncryption::ENCRYPTED;
			} else {
				encrypted = DuckLakeEncryption::UNENCRYPTED;
			}
		}
	}
	if (metadata_catalog_name.empty()) {
		metadata_catalog_name = "__ducklake_metadata_" + name;
	}
	return make_uniq<DuckLakeCatalog>(db, std::move(metadata_catalog_name), info.path, std::move(data_path),
	                                  std::move(schema), encrypted);
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
