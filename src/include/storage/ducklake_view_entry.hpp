//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_view_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "common/index.hpp"
#include "common/enum.hpp"

namespace duckdb {
struct SetCommentInfo;
class DuckLakeTransaction;

class DuckLakeViewEntry : public ViewCatalogEntry {
public:
	DuckLakeViewEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateViewInfo &info, TableIndex view_id,
	                  string view_uuid, string query_sql, TransactionLocalChange transaction_local_change);

public:
	TableIndex GetViewId() const {
		return view_id;
	}
	const string &GetViewUUID() const {
		return view_uuid;
	}
	void SetViewId(TableIndex new_view_id) {
		view_id = new_view_id;
	}
	bool IsTransactionLocal() const {
		return transaction_local_change != TransactionLocalChange::NONE;
	}
	TransactionLocalChange LocalChange() const {
		return transaction_local_change;
	}

public:
	unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info) override;
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;

	const SelectStatement &GetQuery() override;
	bool HasTypes() const override {
		return false;
	}

	string GetQuerySQL();

private:
	mutex parse_lock;
	TableIndex view_id;
	string view_uuid;
	string query_sql;
	TransactionLocalChange transaction_local_change;
};

} // namespace duckdb
