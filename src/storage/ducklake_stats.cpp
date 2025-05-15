#include "storage/ducklake_stats.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

void DuckLakeColumnStats::MergeStats(const DuckLakeColumnStats &new_stats) {
	if (!new_stats.has_null_count) {
		has_null_count = false;
	} else if (has_null_count) {
		// both stats have a null count - add them up
		null_count += new_stats.null_count;
	}
	column_size_bytes += new_stats.column_size_bytes;
	if (!new_stats.has_contains_nan) {
		has_contains_nan = false;
	} else if (has_contains_nan) {
		// both stats have a null count - add them up
		if (new_stats.contains_nan) {
			contains_nan = true;
		}
	}

	if (!new_stats.any_valid) {
		// all values in the source are NULL - don't update min/max
		return;
	}
	if (!any_valid) {
		// all values in the current stats are null - copy the min/max
		min = new_stats.min;
		has_min = new_stats.has_min;
		max = new_stats.max;
		has_max = new_stats.has_max;
		any_valid = true;
		return;
	}

	if (!new_stats.has_min) {
		has_min = false;
	} else if (has_min) {
		// both stats have a min - select the smallest
		if (type.IsNumeric()) {
			// for numerics we need to parse the stats
			auto current_min = Value(min).DefaultCastAs(type);
			auto new_min = Value(new_stats.min).DefaultCastAs(type);
			if (new_min < current_min) {
				min = new_stats.min;
			}
		} else if (new_stats.min < min) {
			// for other types we can compare the strings directly
			min = new_stats.min;
		}
	}

	if (!new_stats.has_max) {
		has_max = false;
	} else if (has_max) {
		// both stats have a min - select the smallest
		if (type.IsNumeric()) {
			// for numerics we need to parse the stats
			auto current_max = Value(max).DefaultCastAs(type);
			auto new_max = Value(new_stats.max).DefaultCastAs(type);
			if (new_max > current_max) {
				max = new_stats.max;
			}
		} else if (new_stats.max > max) {
			// for other types we can compare the strings directly
			max = new_stats.max;
		}
	}
}

void DuckLakeTableStats::MergeStats(FieldIndex col_id, const DuckLakeColumnStats &file_stats) {
	auto entry = column_stats.find(col_id);
	if (entry == column_stats.end()) {
		column_stats.insert(make_pair(col_id, file_stats));
		return;
	}
	// merge the stats
	auto &current_stats = entry->second;
	current_stats.MergeStats(file_stats);
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
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		// we only create stats if we know there are no NaN values
		// FIXME: we can just set Max to NaN instead
		if (has_contains_nan && !contains_nan) {
			return CreateNumericStats();
		}
		return nullptr;
	case LogicalTypeId::VARCHAR:
		return CreateStringStats();
	default:
		return nullptr;
	}
}

} // namespace duckdb
