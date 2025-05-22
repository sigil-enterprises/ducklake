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
		auto lcase = StringUtil::Lower(entry.first);
		if (lcase == "data_path") {
			options.data_path = entry.second.ToString();
		} else if (lcase == "metadata_schema") {
			options.metadata_schema = entry.second.ToString();
		} else if (lcase == "metadata_catalog") {
			options.metadata_database = entry.second.ToString();
		} else if (lcase == "encrypted") {
			if (entry.second.GetValue<bool>()) {
				options.encryption = DuckLakeEncryption::ENCRYPTED;
			} else {
				options.encryption = DuckLakeEncryption::UNENCRYPTED;
			}
		} else if (lcase == "data_inlining_row_limit") {
			options.data_inlining_row_limit = entry.second.GetValue<idx_t>();
		} else if (lcase == "snapshot_version") {
			if (options.at_clause) {
				throw InvalidInputException("Cannot specify both VERSION and TIMESTAMP");
			}
			options.at_clause = make_uniq<BoundAtClause>("version", entry.second.DefaultCastAs(LogicalType::BIGINT));
		} else if (lcase == "snapshot_time") {
			if (options.at_clause) {
				throw InvalidInputException("Cannot specify both VERSION and TIMESTAMP");
			}
			options.at_clause =
			    make_uniq<BoundAtClause>("timestamp", entry.second.DefaultCastAs(LogicalType::TIMESTAMP_TZ));
		} else if (StringUtil::StartsWith(lcase, "meta_")) {
			auto parameter_name = lcase.substr(5);
			options.metadata_parameters[parameter_name] = entry.second;
		} else if (lcase == "readonly" || lcase == "read_only" || lcase == "readwrite" || lcase == "read_write" ||
		           lcase == "type" || lcase == "default_table") {
			// built-in options
			// FIXME: this should really be handled differently upstream
			continue;
		} else {
			throw NotImplementedException("Unsupported option %s for DuckLake", entry.first);
		}
	}
	if (options.metadata_database.empty()) {
		options.metadata_database = "__ducklake_metadata_" + name;
	}
	if (options.at_clause) {
		if (access_mode == AccessMode::READ_WRITE) {
			throw InvalidInputException("SNAPSHOT_VERSION / SNAPSHOT_TIME can only be used in read-only mode");
		}
		access_mode = AccessMode::READ_ONLY;
		db.SetReadOnlyDatabase();
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
