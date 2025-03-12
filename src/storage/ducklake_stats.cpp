#include "storage/ducklake_stats.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

void DuckLakeTableStats::MergeStats(idx_t col_id, const DuckLakeColumnStats &file_stats) {
	auto entry = column_stats.find(col_id);
	if (entry == column_stats.end()) {
		column_stats.insert(make_pair(col_id, file_stats));
		return;
	}
	// merge the stats
	auto &current_stats = entry->second;
	if (!file_stats.has_null_count) {
		current_stats.has_null_count = false;
	} else if (current_stats.has_null_count) {
		// both stats have a null count - add them up
		current_stats.null_count += file_stats.null_count;
	}

	if (!file_stats.any_valid) {
		// all values in the source are NULL - don't update min/max
		return;
	}
	if (!current_stats.any_valid) {
		// all values in the current stats are null - copy the min/max
		current_stats.min = file_stats.min;
		current_stats.has_min = file_stats.has_min;
		current_stats.max = file_stats.max;
		current_stats.has_max = file_stats.has_max;
		current_stats.any_valid = true;
		return;
	}

	if (!file_stats.has_min) {
		current_stats.has_min = false;
	} else if (current_stats.has_min) {
		// both stats have a min - select the smallest
		if (current_stats.type.IsNumeric()) {
			// for numerics we need to parse the stats
			auto current_min = Value(current_stats.min).DefaultCastAs(current_stats.type);
			auto new_min = Value(file_stats.min).DefaultCastAs(current_stats.type);
			if (new_min < current_min) {
				current_stats.min = file_stats.min;
			}
		} else if (file_stats.min < current_stats.min) {
			// for other types we can compare the strings directly
			current_stats.min = file_stats.min;
		}
	}

	if (!file_stats.has_max) {
		current_stats.has_max = false;
	} else if (current_stats.has_max) {
		// both stats have a min - select the smallest
		if (current_stats.type.IsNumeric()) {
			// for numerics we need to parse the stats
			auto current_min = Value(current_stats.max).DefaultCastAs(current_stats.type);
			auto new_min = Value(file_stats.max).DefaultCastAs(current_stats.type);
			if (new_min > current_min) {
				current_stats.max = file_stats.max;
			}
		} else if (file_stats.max > current_stats.max) {
			// for other types we can compare the strings directly
			current_stats.max = file_stats.max;
		}
	}
}

unique_ptr<BaseStatistics> DuckLakeColumnStats::CreateNumericStats() const {
	if (!has_min || !has_max) {
		return nullptr;
	}
	auto stats = NumericStats::CreateEmpty(type);
	// set min
	Value min_val(min);
	NumericStats::SetMin(stats, min_val.DefaultCastAs(type));
	// set max
	Value max_val(max);
	NumericStats::SetMax(stats, max_val.DefaultCastAs(type));
	// set null count
	if (!has_null_count || null_count > 0) {
		stats.SetHasNull();
	}
	stats.SetHasNoNull();
	return stats.ToUnique();
}

unique_ptr<BaseStatistics> DuckLakeColumnStats::CreateStringStats() const {
	if (!has_min || !has_max) {
		return nullptr;
	}
	auto stats = StringStats::CreateEmpty(type);

	StringStats::Update(stats, string_t(min));
	StringStats::Update(stats, string_t(max));
	StringStats::ResetMaxStringLength(stats);
	StringStats::SetContainsUnicode(stats);
	// set null count
	if (!has_null_count || null_count > 0) {
		stats.SetHasNull();
	}
	stats.SetHasNoNull();
	return stats.ToUnique();
}

unique_ptr<BaseStatistics> DuckLakeColumnStats::ToStats() const {
	if (type.IsNumeric()) {
		return CreateNumericStats();
	}
	switch (type.id()) {
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return CreateNumericStats();
	case LogicalTypeId::VARCHAR:
		return CreateStringStats();
	default:
		return nullptr;
	}
}

} // namespace duckdb
