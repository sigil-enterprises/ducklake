//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/index.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {
class DataChunk;
class ColumnList;
class DuckLakeMetadataManager;
class FileSystem;
class TableFilter;
class DynamicFilter;

struct ParsedCatalogEntry {
	string schema;
	string name;
};

class DuckLakeUtil {
public:
	static string ParseQuotedValue(const string &input, idx_t &pos);
	static string ToQuotedList(const vector<string> &input, char list_separator = ',');
	static vector<string> ParseQuotedList(const string &input, char list_separator = ',');
	static string SQLIdentifierToString(const string &text);
	static string SQLLiteralToString(const string &text);
	static string WrappedEncryptionKeyLiteral(const string &wrapped_base64, bool file_has_key);
	// >>> FORK-LOCAL (sigil-enterprises): shared guard extracted so the
	// Appender fast path (WriteNewDataFilesWithAppender) and the SQL-batch
	// path (WriteNewDataFilesSqlBatch, via WrappedEncryptionKeyLiteral above)
	// enforce the empty-wrapped-value refusal from ONE place (bench#96).
	// PRIVATE-FORK ONLY. Never cherry-pick this method upstream.
	//
	// Returns false (nothing to write - the caller should write SQL NULL /
	// Appender Value()) when wrapped_base64 is empty and the file has no key.
	// Throws InternalException, same as WrappedEncryptionKeyLiteral, when
	// wrapped_base64 is empty but the file DOES have a key. Otherwise returns
	// true with out_value set to the raw (unescaped) wrapped value.
	static bool WrappedEncryptionKeyOrThrow(const string &wrapped_base64, bool file_has_key, string &out_value);
	// <<< FORK-LOCAL (sigil-enterprises) <<<
	static string StatsToString(const string &text);
	static string ValueToSQL(DuckLakeMetadataManager &metadata_manager, ClientContext &context, const Value &val);

	static ParsedCatalogEntry ParseCatalogEntry(const string &input);
	static string JoinPath(FileSystem &fs, const string &a, const string &b);

	static DynamicFilter *GetOptionalDynamicFilter(const TableFilter &filter);

	//! Create the data path directory if it does not yet exist
	static void EnsureDirectoryExists(FileSystem &fs, const string &data_path);

	//! Replace occurrences of `from` with `to`, skipping content inside
	//! single-quoted string literals and double-quoted identifiers.
	static string ReplaceSkippingQuotes(const string &sql, const string &from, const string &to);

	//! Returns true if the given column name conflicts with inlined data system
	//! columns
	static bool IsInlinedSystemColumn(const string &name);

	static string OptionalIdxOrNull(const optional_idx &v);

	static string MappingIdOrNull(const MappingIndex &m);

	static string EncryptionKeyLiteral(const string &key);

	static const char *BoolLiteral(bool v);

	static string PartitionValueLiteral(const Value &v);

	static string ChunkRowToSQL(DuckLakeMetadataManager &metadata_manager, ClientContext &context, DataChunk &chunk,
	                            idx_t row);
	//! Throws if any column in the list conflicts with inlined data system
	//! columns
	static void ValidateNoInlinedSystemColumns(const ColumnList &columns, const string &table_name = "");
};

} // namespace duckdb
