#include "ducklake_catalog.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/common/multi_file_reader_function.hpp"

namespace duckdb {

// traverse the LogicalDelete to find the DUCKLAKE_SCAN
// extract the file list
// create a UNION of operations -> COPY TO FILE (filename, file_row_number) with the delete file for each file
// (filename does not need to come from the reader)

LogicalGet &ExtractLogicalGet(reference<LogicalOperator> op) {
	while(op.get().type != LogicalOperatorType::LOGICAL_GET) {
		if (op.get().type == LogicalOperatorType::LOGICAL_FILTER) {
			op = *op.get().children[0];
		} else {
			throw NotImplementedException("Unimplemented logical operator type in DuckLake Delete");
		}
	}
	return op.get().Cast<LogicalGet>();
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) {
	auto &get = ExtractLogicalGet(*op.children[0]);
	auto &bind_data = get.bind_data->Cast<MultiFileBindData>();
	auto files = bind_data.file_list->GetAllFiles();


	throw InternalException("Unsupported DuckLake function");
}

unique_ptr<PhysicalOperator> DuckLakeCatalog::PlanDelete(ClientContext &context, LogicalDelete &op,
														 unique_ptr<PhysicalOperator> plan) {
	throw InternalException("Unsupported DuckLake function");
}

}

