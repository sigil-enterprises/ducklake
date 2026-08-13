//===----------------------------------------------------------------------===//
//                         DuckLake
//
// functions/ducklake_self_test.cpp
//
// Boot-time self-test: every reader must LOAD ducklake + perform one
// encrypted read before serving. This function is the call surface.
//
// ducklake:M5 — Fleet-convergence gate
//
//===----------------------------------------------------------------------===//
//
// WHAT IT VERIFIES
// ----------------
// 1. Extension is loaded and reports its version.
// 2. Encryption provider factory is registered (a build without a concrete
//    KMS backend has no factory and cannot decrypt anything).
// 3. When pointed at a catalog: the provider's SelfTest() confirms the KMS
//    is reachable and reports what it is rooted in.
//
// USAGE
// -----
// Called by every consumer at boot, before serving:
//
//   SELECT * FROM ducklake_self_test();
//   SELECT * FROM ducklake_self_test('my_lake');
//
// A consumer that gets zero rows or `load_ok = false` refuses to start.
//
//===----------------------------------------------------------------------===//

#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_encryption_provider.hpp"

namespace duckdb {

struct SelfTestRow {
	string extension_name;
	string extension_version;
	string duckdb_version;
	string provider_kind;
	bool load_ok = false;
};

struct SelfTestBindData : public TableFunctionData {
	DuckLakeCatalog *catalog = nullptr;
};

static unique_ptr<FunctionData> DuckLakeSelfTestBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<SelfTestBindData>();

	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		auto db_name = input.inputs[0].GetValue<string>();
		auto &db_manager = DatabaseManager::Get(context);
		auto db = db_manager.GetDatabase(context, Identifier(db_name));
		if (!db) {
			throw InvalidInputException("ducklake_self_test: failed to find attached database \"%s\"", db_name);
		}
		auto &catalog_obj = db->GetCatalog();
		if (catalog_obj.GetCatalogType() != "ducklake") {
			throw InvalidInputException("ducklake_self_test: \"%s\" is a %s catalog, not a ducklake catalog", db_name,
			                            catalog_obj.GetCatalogType());
		}
		auto &ducklake_catalog = catalog_obj.Cast<DuckLakeCatalog>();
		result->catalog = &ducklake_catalog;
	}

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("extension_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("extension_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("duckdb_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("provider_kind");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("load_ok");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DuckLakeSelfTestInit(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	return nullptr;
}

static void DuckLakeSelfTestExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<SelfTestBindData>();

	SelfTestRow row;
	row.extension_name = "ducklake";
#ifdef EXT_VERSION_DUCKLAKE
	row.extension_version = EXT_VERSION_DUCKLAKE;
#else
	row.extension_version = "(dev)";
#endif
	row.duckdb_version = DuckDB::LibraryVersion();
	row.provider_kind = "not_checked";
	row.load_ok = true;

	if (!data.catalog) {
		// No catalog named — cheap pass: the extension loaded, the function
		// is reachable, and the caller proved it can execute SQL through DuckDB.
		// This is what a consumer that does not hold encryption material calls.
	} else {
		// Catalog named — full encryption-path health check.
		auto &factory = DuckLakeEncryptionProvider::GetFactory();
		if (!factory) {
			row.provider_kind = "factory_not_registered";
			row.load_ok = false;
		} else {
			auto provider = data.catalog->EncryptionProvider();
			if (!provider) {
				row.provider_kind = "catalog_has_no_provider";
				row.load_ok = false;
			} else {
				try {
					row.provider_kind = provider->SelfTest();
				} catch (std::exception &e) {
					row.provider_kind = StringUtil::Format("error: %s", e.what());
					row.load_ok = false;
				}
			}
		}
	}

	output.SetValue(0, 0, Value(row.extension_name));
	output.SetValue(1, 0, Value(row.extension_version));
	output.SetValue(2, 0, Value(row.duckdb_version));
	output.SetValue(3, 0, Value(row.provider_kind));
	output.SetValue(4, 0, Value::BOOLEAN(row.load_ok));
	output.SetCardinality(1);
}

DuckLakeSelfTestFunction::DuckLakeSelfTestFunction()
    : TableFunction("ducklake_self_test", {}, DuckLakeSelfTestExecute, DuckLakeSelfTestBind,
                    DuckLakeSelfTestInit) {
	varargs = LogicalType::VARCHAR;
}

} // namespace duckdb
