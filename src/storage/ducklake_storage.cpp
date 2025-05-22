#include "duckdb.hpp"

#include "storage/ducklake_storage.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog> DuckLakeAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AccessMode access_mode) {
	DuckLakeOptions options;
	options.metadata_path = info.path;
	for (auto &entry : info.options) {
		if (StringUtil::CIEquals(entry.first, "data_path")) {
			options.data_path = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "metadata_schema")) {
			options.metadata_schema = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "metadata_catalog")) {
			options.metadata_database = entry.second.ToString();
		} else if (StringUtil::CIEquals(entry.first, "encrypted")) {
			if (entry.second.GetValue<bool>()) {
				options.encryption = DuckLakeEncryption::ENCRYPTED;
			} else {
				options.encryption = DuckLakeEncryption::UNENCRYPTED;
			}
		} else if (StringUtil::CIEquals(entry.first, "data_inlining_row_limit")) {
			options.data_inlining_row_limit = entry.second.GetValue<idx_t>();
		}
	}
	if (options.metadata_database.empty()) {
		options.metadata_database = "__ducklake_metadata_" + name;
	}
	options.access_mode = access_mode;
	return make_uniq<DuckLakeCatalog>(db, std::move(options));
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
