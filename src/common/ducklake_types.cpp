#include "common/ducklake_types.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"

namespace duckdb {

struct DefaultType {
	const char *name;
	LogicalTypeId id;
};

using ducklake_type_array = std::array<DefaultType, 33>;

static constexpr const ducklake_type_array DUCKLAKE_TYPES {{{"boolean", LogicalTypeId::BOOLEAN},
                                                            {"int8", LogicalTypeId::TINYINT},
                                                            {"int16", LogicalTypeId::SMALLINT},
                                                            {"int32", LogicalTypeId::INTEGER},
                                                            {"int64", LogicalTypeId::BIGINT},
                                                            {"int128", LogicalTypeId::HUGEINT},
                                                            {"uint8", LogicalTypeId::UTINYINT},
                                                            {"uint16", LogicalTypeId::USMALLINT},
                                                            {"uint32", LogicalTypeId::UINTEGER},
                                                            {"uint64", LogicalTypeId::UBIGINT},
                                                            {"uint128", LogicalTypeId::UHUGEINT},
                                                            {"float32", LogicalTypeId::FLOAT},
                                                            {"float64", LogicalTypeId::DOUBLE},
                                                            {"decimal", LogicalTypeId::DECIMAL},
                                                            {"time", LogicalTypeId::TIME},
                                                            {"time_ns", LogicalTypeId::TIME_NS},
                                                            {"date", LogicalTypeId::DATE},
                                                            {"timestamp", LogicalTypeId::TIMESTAMP},
                                                            {"timestamp_us", LogicalTypeId::TIMESTAMP},
                                                            {"timestamp_ms", LogicalTypeId::TIMESTAMP_MS},
                                                            {"timestamp_ns", LogicalTypeId::TIMESTAMP_NS},
                                                            {"timestamp_s", LogicalTypeId::TIMESTAMP_SEC},
                                                            {"timestamptz", LogicalTypeId::TIMESTAMP_TZ},
                                                            {"timestamptz_ns", LogicalTypeId::TIMESTAMP_TZ_NS},
                                                            {"timetz", LogicalTypeId::TIME_TZ},
                                                            {"interval", LogicalTypeId::INTERVAL},
                                                            {"varchar", LogicalTypeId::VARCHAR},
                                                            {"blob", LogicalTypeId::BLOB},
                                                            {"uuid", LogicalTypeId::UUID},
                                                            {"struct", LogicalTypeId::STRUCT},
                                                            {"map", LogicalTypeId::MAP},
                                                            {"list", LogicalTypeId::LIST},
                                                            {"unknown", LogicalTypeId::UNKNOWN}}};

static LogicalType ParseBaseType(const string &str) {
	for (auto &ducklake_type : DUCKLAKE_TYPES) {
		if (StringUtil::CIEquals(str, ducklake_type.name)) {
			return ducklake_type.id;
		}
	}

	if (StringUtil::CIEquals(str, "json")) {
		return LogicalType::JSON();
	}
	if (StringUtil::CIEquals(str, "variant")) {
		return LogicalType::VARIANT();
	}
	if (StringUtil::CIEquals(str, "geometry")) {
		return LogicalType::GEOMETRY();
	}

	throw InvalidInputException("Failed to parse DuckLake type - unsupported type '%s'", str);
}

static string ToStringBaseType(const LogicalType &type) {
	for (auto &ducklake_type : DUCKLAKE_TYPES) {
		if (type.id() == ducklake_type.id) {
			return ducklake_type.name;
		}
	}
	throw InvalidInputException("Failed to convert DuckDB type to DuckLake - unsupported type %s", type);
}

bool DuckLakeTypes::RequiresCast(const LogicalType &type) {
	// There are no types that requires casts as of DuckDB v1.5
	return false;
}

bool DuckLakeTypes::RequiresCast(const vector<LogicalType> &types) {
	for (auto &type : types) {
		if (RequiresCast(type)) {
			return true;
		}
	}
	return false;
}

LogicalType DuckLakeTypes::GetCastedType(const LogicalType &type) {
	// There are no types that requires casts as of DuckDB v1.5
	return type;
}

LogicalType DuckLakeTypes::FromString(const string &type) {
	if (StringUtil::StartsWith(type, "decimal(") && StringUtil::EndsWith(type, ")")) {
		// decimal - parse width/scale
		string decimal_members_str = type.substr(8, type.size() - 9);
		vector<string> decimal_members_vect = StringUtil::SplitWithParentheses(decimal_members_str);
		if (decimal_members_vect.size() != 2) {
			throw NotImplementedException("Invalid DECIMAL type - expected width and scale");
		}
		auto width = std::stoull(decimal_members_vect[0]);
		auto scale = std::stoull(decimal_members_vect[1]);
		return LogicalType::DECIMAL(width, scale);
	}
	return ParseBaseType(type);
}

string DuckLakeTypes::ToString(const LogicalType &type) {
	if (type.HasAlias()) {
		if (type.IsJSONType()) {
			return "json";
		}
		if (type.id() == LogicalTypeId::UNBOUND) {
			const auto type_name = type.GetAlias();
			if (StringUtil::Lower(type_name) == "json") {
				return "json";
			}
		}
		throw InvalidInputException("Unsupported user-defined type");
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		// TUPLE is an unnamed struct that shares STRUCT's physical representation (duckdb-core) -
		// DuckLake has no separate storage type for it, so store it as "struct"
		return "struct";
	case LogicalTypeId::VARIANT:
		return "variant";
	case LogicalTypeId::GEOMETRY:
		return "geometry";
	case LogicalTypeId::LIST:
		return "list";
	case LogicalTypeId::MAP:
		return "map";
	case LogicalTypeId::DECIMAL:
		return "decimal(" + to_string(DecimalType::GetWidth(type)) + "," + to_string(DecimalType::GetScale(type)) + ")";
	case LogicalTypeId::VARCHAR:
		if (!StringType::GetCollation(type).empty()) {
			throw InvalidInputException("Collations are not supported in DuckLake storage");
		}
		return ToStringBaseType(type);
	default:
		return ToStringBaseType(type);
	}
}

void DuckLakeTypes::CheckSupportedType(const LogicalType &type) {
	TypeVisitor::VisitReplace(type, [](const LogicalType &type) {
		DuckLakeTypes::ToString(type);
		return type;
	});
}

void DuckLakeTypes::CastEvolvedVector(ClientContext &context, Vector &source, Vector &target, idx_t count) {
	auto &source_type = source.GetType();
	auto &target_type = target.GetType();
	if (source_type == target_type) {
		target.Reference(source);
		return;
	}
	auto is_struct_like = [](const LogicalType &t) {
		return t.id() == LogicalTypeId::STRUCT || t.id() == LogicalTypeId::TUPLE;
	};
	if (is_struct_like(source_type) && is_struct_like(target_type)) {
		source.Flatten(count);
		auto &source_children = StructVector::GetEntries(source);
		auto &target_children = StructVector::GetEntries(target);
		auto &source_child_types = StructType::GetChildTypes(source_type);
		auto &target_child_types = StructType::GetChildTypes(target_type);
		for (idx_t target_idx = 0; target_idx < target_children.size(); target_idx++) {
			auto &target_name = target_child_types[target_idx].first;
			bool found = false;
			for (idx_t source_idx = 0; source_idx < source_children.size(); source_idx++) {
				if (source_child_types[source_idx].first == target_name) {
					CastEvolvedVector(context, source_children[source_idx], target_children[target_idx], count);
					found = true;
					break;
				}
			}
			if (!found) {
				// field did not exist in the old (source) struct layout - fill with NULL
				target_children[target_idx].SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(target_children[target_idx], true);
			}
		}
		target.SetVectorType(VectorType::FLAT_VECTOR);
		FlatVector::SetValidity(target, FlatVector::Validity(source));
		return;
	}
	if (source_type.id() == LogicalTypeId::LIST && target_type.id() == LogicalTypeId::LIST) {
		source.Flatten(count);
		auto list_size = ListVector::GetListSize(source);
		ListVector::Reserve(target, list_size);
		ListVector::SetListSize(target, list_size);
		target.SetVectorType(VectorType::FLAT_VECTOR);
		memcpy(FlatVector::GetDataMutable<list_entry_t>(target), FlatVector::GetData<list_entry_t>(source),
		      count * sizeof(list_entry_t));
		FlatVector::SetValidity(target, FlatVector::Validity(source));
		auto &source_child = ListVector::GetEntry(source);
		auto &target_child = ListVector::GetEntry(target);
		CastEvolvedVector(context, source_child, target_child, list_size);
		return;
	}
	// fall back to the regular (name/id agnostic) cast for everything else (primitives, and nested types
	// that did not change shape/name in an incompatible way)
	VectorOperations::Cast(context, source, target, count);
}

} // namespace duckdb
