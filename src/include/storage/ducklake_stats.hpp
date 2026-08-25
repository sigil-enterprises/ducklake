//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_extra_stats.hpp"

namespace duckdb {
class BaseStatistics;
struct DuckLakeDataFile;

//! Returns true for types that require value-based (not lexicographic string) comparison for min/max stats
inline bool RequiresValueComparison(const LogicalType &type) {
	return type.IsNumeric() || type.IsTemporal() || type.id() == LogicalTypeId::BOOLEAN;
}

struct DuckLakeColumnStats;
struct DuckLakeGlobalColumnStatsInfo;

struct DuckLakeColumnStats {
	explicit DuckLakeColumnStats(LogicalType type_p);

	// Copy constructor
	DuckLakeColumnStats(const DuckLakeColumnStats &other);
	DuckLakeColumnStats &operator=(const DuckLakeColumnStats &other);
	DuckLakeColumnStats(DuckLakeColumnStats &&other) noexcept = default;
	DuckLakeColumnStats &operator=(DuckLakeColumnStats &&other) noexcept = default;

	LogicalType type;
	string min;
	string max;
	idx_t null_count = 0;
	idx_t num_values = 0;
	idx_t column_size_bytes = 0;
	bool contains_nan = false;
	bool has_null_count = false;
	bool has_num_values = false;
	bool has_min = false;
	bool has_max = false;
	bool any_valid = true;
	bool has_contains_nan = false;
	// Transient (never serialised): set by RedactValues() on the file-stats side
	// so the commit-time table-wide merge can tell a redacted-empty extra_stats
	// (which must CLEAR the accumulated bound) from a legitimately-empty one
	// (which must leave it alone). Re-applied on every write, so it does not
	// need to survive the write -> read round-trip.
	bool redacted = false;

	bool AnyValid() const {
		if (has_num_values && has_null_count) {
			return num_values > null_count;
		}
		return any_valid;
	}

	unique_ptr<DuckLakeColumnExtraStats> extra_stats;

public:
	static DuckLakeColumnStats FromGlobalStats(const LogicalType &type, const DuckLakeGlobalColumnStatsInfo &col);
	unique_ptr<BaseStatistics> ToStats() const;
	void MergeStats(const DuckLakeColumnStats &new_stats);

	// >>> FORK-LOCAL (sigil-enterprises): the envelope forbids column VALUES in the catalog. >>>
	// PRIVATE-FORK ONLY. Never cherry-pick this declaration upstream.
	//
	//! Drop every VALUE-BEARING statistic, keeping the counts. min/max and the
	//! extra stats are actual column values - on a narrow-range or
	//! low-cardinality column min/max IS the data - and the metadata catalog
	//! stores them as plaintext VARCHAR. value_count, null_count,
	//! column_size_bytes and contains_nan are counts about the data, not values
	//! from it, and are deliberately KEPT: they are what still answers count(*)
	//! and NULL-based pruning without opening an encrypted Parquet file.
	//!
	//! `any_valid` is deliberately NOT cleared. MergeStats treats a source with
	//! no valid values as "nothing to merge" and returns EARLY, which would
	//! leave a stale min/max standing in the table-wide stats of a lake that
	//! predates the envelope. Leaving it set makes the merge run and clear the
	//! bound.
	void RedactValues();
	// <<< FORK-LOCAL (sigil-enterprises) <<<

private:
	unique_ptr<BaseStatistics> CreateNumericStats() const;
	unique_ptr<BaseStatistics> CreateStringStats() const;
	unique_ptr<BaseStatistics> CreateVariantStats() const;
	unique_ptr<BaseStatistics> CreateGeometryStats() const;
};

//! These are the global, table-wide stats
struct DuckLakeTableStats {
	idx_t record_count = 0;
	idx_t table_size_bytes = 0;
	idx_t next_row_id = 0;
	map<FieldIndex, DuckLakeColumnStats> column_stats;

	void MergeStats(FieldIndex col_id, const DuckLakeColumnStats &file_stats);

	void MergeFileStats(const DuckLakeDataFile &file);
};

struct DuckLakeStats {
	map<DuckLakeTableIndex, unique_ptr<DuckLakeTableStats>> table_stats;
};

} // namespace duckdb
