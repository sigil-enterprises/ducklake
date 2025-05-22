#include "duckdb/catalog/default/default_table_functions.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"

namespace duckdb {

// clang-format off
static const DefaultTableMacro ducklake_table_macros[] = {
	{DEFAULT_SCHEMA, "snapshots", {nullptr}, {{nullptr, nullptr}},  "FROM ducklake_snapshots({CATALOG})"},
	{DEFAULT_SCHEMA, "table_info", {nullptr}, {{nullptr, nullptr}},  "FROM ducklake_table_info({CATALOG})"},
	{nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}
};
// clang-format on

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::LoadBuiltInFunction(DefaultTableMacro macro) {
	string macro_definition =
	    StringUtil::Replace(macro.macro, "{CATALOG}", KeywordHelper::WriteQuoted(catalog.GetName(), '\''));
	macro.macro = macro_definition.c_str();
	auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(macro);
	auto table_macro =
	    make_uniq_base<CatalogEntry, TableMacroCatalogEntry>(catalog, *this, info->Cast<CreateMacroInfo>());
	auto result = table_macro.get();
	default_function_map.emplace(macro.name, std::move(table_macro));
	return result;
}

optional_ptr<CatalogEntry> DuckLakeSchemaEntry::TryLoadBuiltInFunction(const string &entry_name) {
	lock_guard<mutex> guard(default_function_lock);
	auto entry = default_function_map.find(entry_name);
	if (entry != default_function_map.end()) {
		return entry->second.get();
	}
	for (idx_t index = 0; ducklake_table_macros[index].name != nullptr; index++) {
		if (StringUtil::CIEquals(ducklake_table_macros[index].name, entry_name)) {
			return LoadBuiltInFunction(ducklake_table_macros[index]);
		}
	}
	return nullptr;
}

} // namespace duckdb
