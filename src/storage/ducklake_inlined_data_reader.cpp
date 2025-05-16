#include "storage/ducklake_inlined_data_reader.hpp"
#include "storage/ducklake_multi_file_reader.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {

DuckLakeInlinedDataReader::DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info,
                                                     string table_name_p, vector<MultiFileColumnDefinition> columns_p)
    : BaseFileReader(info), read_info(read_info), table_name(std::move(table_name_p)) {
	columns = std::move(columns_p);
}
DuckLakeInlinedDataReader::DuckLakeInlinedDataReader(DuckLakeFunctionInfo &read_info, const OpenFileInfo &info,
                                                     shared_ptr<DuckLakeInlinedData> data_p,
                                                     vector<MultiFileColumnDefinition> columns_p)
    : BaseFileReader(info), read_info(read_info), data(std::move(data_p)) {
	columns = std::move(columns_p);
}

bool DuckLakeInlinedDataReader::TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
                                                  LocalTableFunctionState &lstate) {
	{
		// check if we are the reader responsible for scanning
		lock_guard<mutex> guard(lock);
		if (initialized_scan) {
			return false;
		}
		initialized_scan = true;
	}
	if (!expression_map.empty()) {
		throw InternalException("FIXME: support expression_map");
	}

	if (!data) {
		// scanning data from a table - read it from the metadata catalog
		auto &metadata_manager = read_info.transaction.GetMetadataManager();

		// push the projections directly into the read
		vector<string> columns_to_read;
		for (auto &column_id : column_indexes) {
			auto index = column_id.GetPrimaryIndex();
			auto &col = columns[index];
			if (!col.identifier.IsNull() && col.identifier.type().id() == LogicalTypeId::INTEGER) {
				auto identifier = IntegerValue::Get(col.identifier);
				string virtual_column;
				switch (identifier) {
				case MultiFileReader::ORDINAL_FIELD_ID:
				case MultiFileReader::ROW_ID_FIELD_ID:
					virtual_column = "row_id";
					break;
				case MultiFileReader::LAST_UPDATED_SEQUENCE_NUMBER_ID:
					virtual_column = "begin_snapshot";
					break;
				default:
					break;
				}
				if (!virtual_column.empty()) {
					columns_to_read.push_back(virtual_column);
					continue;
				}
			}
			columns_to_read.push_back(columns[index].name);
		}
		data = metadata_manager.ReadInlinedData(read_info.snapshot, table_name, columns_to_read);
		data->data->InitializeScan(state);
	} else {
		// scanning from transaction-local data - we already have the data
		// push the projections ivector<column_t> column_ids;nto the scan
		vector<LogicalType> scan_types;
		auto &types = data->data->Types();
		for (idx_t i = 0; i < column_indexes.size(); ++i) {
			auto &column_id = column_indexes[i];
			auto col_id = column_id.GetPrimaryIndex();
			if (col_id >= types.size()) {
				is_virtual.push_back(true);
				continue;
			}
			scan_types.push_back(types[col_id]);
			scan_column_ids.push_back(col_id);
			is_virtual.push_back(false);
		}
		if (!scan_types.empty()) {
			scan_types.push_back(types[0]);
			scan_column_ids.push_back(0);
		}
		scan_chunk.Initialize(context, scan_types);

		data->data->InitializeScan(state, scan_column_ids);
	}
	return true;
}

void DuckLakeInlinedDataReader::Scan(ClientContext &context, GlobalTableFunctionState &global_state,
                                     LocalTableFunctionState &local_state, DataChunk &chunk) {
	if (!is_virtual.empty()) {
		scan_chunk.Reset();
		data->data->Scan(state, scan_chunk);

		idx_t source_idx = 0;
		for (idx_t c = 0; c < is_virtual.size(); c++) {
			if (is_virtual[c]) {
				auto row_id_data = FlatVector::GetData<int64_t>(chunk.data[c]);
				for (idx_t r = 0; r < scan_chunk.size(); r++) {
					row_id_data[r] = file_row_number + r;
				}
				continue;
			}
			auto column_id = source_idx++;
			chunk.data[c].Reference(scan_chunk.data[column_id]);
		}
		file_row_number += scan_chunk.size();
		chunk.SetCardinality(scan_chunk.size());
	} else {
		data->data->Scan(state, chunk);
	}
	if (filters) {
		SelectionVector sel;
		idx_t approved_tuple_count = chunk.size();
		for (auto &entry : filters->filters) {
			auto column_id = entry.first;
			auto &vec = chunk.data[column_id];

			UnifiedVectorFormat vdata;
			vec.ToUnifiedFormat(chunk.size(), vdata);

			auto &filter = *entry.second;
			auto filter_state = TableFilterState::Initialize(context, filter);

			approved_tuple_count = ColumnSegment::FilterSelection(sel, vec, vdata, filter, *filter_state, chunk.size(),
			                                                      approved_tuple_count);
		}
		chunk.Slice(sel, approved_tuple_count);
	}
}

void DuckLakeInlinedDataReader::AddVirtualColumn(column_t virtual_column_id) {
	if (virtual_column_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
		columns.back().identifier = Value::INTEGER(MultiFileReader::ORDINAL_FIELD_ID);
	} else {
		throw InternalException("Unsupported virtual column id %d for inlined data reader", virtual_column_id);
	}
}

string DuckLakeInlinedDataReader::GetReaderType() const {
	return "DuckLake Inlined Data";
}

} // namespace duckdb
