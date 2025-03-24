#include "storage/ducklake_view_entry.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

namespace duckdb {

DuckLakeViewEntry::DuckLakeViewEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateViewInfo &info,
                                     TableIndex view_id, string view_uuid_p, string query_sql_p,
                                     TransactionLocalChange transaction_local_change)
    : ViewCatalogEntry(catalog, schema, info), view_id(view_id), view_uuid(std::move(view_uuid_p)),
      query_sql(std::move(query_sql_p)), transaction_local_change(transaction_local_change) {
}

unique_ptr<CatalogEntry> DuckLakeViewEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
	switch (info.type) {
	case AlterType::SET_COMMENT: {
		auto &alter = info.Cast<SetCommentInfo>();
		auto info = GetInfo();
		info->comment = alter.comment_value;
		auto &view_info = info->Cast<CreateViewInfo>();
		auto new_view = make_uniq<DuckLakeViewEntry>(catalog, schema, view_info, GetViewId(), GetViewUUID(), query_sql,
		                                             TransactionLocalChange::SET_COMMENT);
		return std::move(new_view);
		break;
	}
	default:
		throw NotImplementedException("Unsupported ALTER type for VIEW");
	}
}

unique_ptr<CatalogEntry> DuckLakeViewEntry::Copy(ClientContext &context) const {
	D_ASSERT(!internal);
	auto create_info = GetInfo();

	return make_uniq<DuckLakeViewEntry>(catalog, schema, create_info->Cast<CreateViewInfo>(), view_id, view_uuid,
	                                    query_sql, transaction_local_change);
}

const SelectStatement &DuckLakeViewEntry::GetQuery() {
	lock_guard<mutex> l(parse_lock);
	if (!query) {
		// parse the query
		Parser parser;
		parser.ParseQuery(query_sql);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			throw InvalidInputException("Invalid input for view - view must have a single SELECT statement: \"%s\"",
			                            query_sql);
		}
		query = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	}
	return *query;
}

string DuckLakeViewEntry::GetQuerySQL() {
	return query_sql;
}

} // namespace duckdb
