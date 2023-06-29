#include <duckpgq_extension.hpp>
#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckpgq/common.hpp"
#include "duckpgq/duckpgq_functions.hpp"

namespace duckdb {

static bool IterativeLength(int64_t v_size, int64_t *v, vector<int64_t> &e, vector<std::bitset<LANE_LIMIT>> &seen,
                            vector<std::bitset<LANE_LIMIT>> &visit, vector<std::bitset<LANE_LIMIT>> &next) {
	bool change = false;
	for (auto i = 0; i < v_size; i++) {
		next[i] = 0;
	}
	for (auto i = 0; i < v_size; i++) {
		if (visit[i].any()) {
			for (auto offset = v[i]; offset < v[i + 1]; offset++) {
				auto n = e[offset];
				next[n] = next[n] | visit[i];
			}
		}
	}
	for (auto i = 0; i < v_size; i++) {
		next[i] = next[i] & ~seen[i];
		seen[i] = seen[i] | next[i];
		change |= next[i].any();
	}
	return change;
}

static void IterativeLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (IterativeLengthFunctionData &)*func_expr.bind_info;
	auto sqlpgq_state_entry = info.context.registered_state.find("sqlpgq");
	if (sqlpgq_state_entry == info.context.registered_state.end()) {
		//! Wondering how you can get here if the extension wasn't loaded, but leaving this check in anyways
		throw MissingExtensionException("The SQL/PGQ extension has not been loaded");
	}
	auto sqlpgq_state = reinterpret_cast<DuckPGQState *>(sqlpgq_state_entry->second.get());

	D_ASSERT(sqlpgq_state->csr_list[info.csr_id]);

	if ((uint64_t)info.csr_id + 1 > sqlpgq_state->csr_list.size()) {
		throw ConstraintException("Invalid ID");
	}
	auto csr_entry = sqlpgq_state->csr_list.find((uint64_t)info.csr_id);
	if (csr_entry == sqlpgq_state->csr_list.end()) {
		throw ConstraintException("Need to initialize CSR before doing shortest path");
	}

	if (!(csr_entry->second->initialized_v && csr_entry->second->initialized_e)) {
		throw ConstraintException("Need to initialize CSR before doing shortest path");
	}
	int64_t v_size = args.data[1].GetValue(0).GetValue<int64_t>();
	int64_t *v = (int64_t *)sqlpgq_state->csr_list[info.csr_id]->v;
	vector<int64_t> &e = sqlpgq_state->csr_list[info.csr_id]->e;

	// get src and dst vectors for searches
	auto &src = args.data[2];
	auto &dst = args.data[3];
	UnifiedVectorFormat vdata_src;
	UnifiedVectorFormat vdata_dst;
	src.ToUnifiedFormat(args.size(), vdata_src);
	dst.ToUnifiedFormat(args.size(), vdata_dst);
	auto src_data = (int64_t *)vdata_src.data;
	auto dst_data = (int64_t *)vdata_dst.data;

	ValidityMask &result_validity = FlatVector::Validity(result);

	// create result vector
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<int64_t>(result);

	// create temp SIMD arrays
	vector<std::bitset<LANE_LIMIT>> seen(v_size);
	vector<std::bitset<LANE_LIMIT>> visit1(v_size);
	vector<std::bitset<LANE_LIMIT>> visit2(v_size);

	// maps lane to search number
	short lane_to_num[LANE_LIMIT];
	for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
		lane_to_num[lane] = -1; // inactive
	}

	idx_t started_searches = 0;
	while (started_searches < args.size()) {

		// empty visit vectors
		for (auto i = 0; i < v_size; i++) {
			seen[i] = 0;
			visit1[i] = 0;
		}

		// add search jobs to free lanes
		uint64_t active = 0;
		for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
			lane_to_num[lane] = -1;
			while (started_searches < args.size()) {
				int64_t search_num = started_searches++;
				int64_t src_pos = vdata_src.sel->get_index(search_num);
				int64_t dst_pos = vdata_dst.sel->get_index(search_num);
				if (!vdata_src.validity.RowIsValid(src_pos)) {
					result_validity.SetInvalid(search_num);
					result_data[search_num] = (uint64_t)-1; /* no path */
				} else if (src_data[src_pos] == dst_data[dst_pos]) {
					result_data[search_num] = (uint64_t)0; // path of length 0 does not require a search
				} else {
					visit1[src_data[src_pos]][lane] = true;
					lane_to_num[lane] = search_num; // active lane
					active++;
					break;
				}
			}
		}

		// make passes while a lane is still active
		for (int64_t iter = 1; active; iter++) {
			if (!IterativeLength(v_size, v, e, seen, (iter & 1) ? visit1 : visit2, (iter & 1) ? visit2 : visit1)) {
				break;
			}
			// detect lanes that finished
			for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
				int64_t search_num = lane_to_num[lane];
				if (search_num >= 0) { // active lane
					int64_t dst_pos = vdata_dst.sel->get_index(search_num);
					if (seen[dst_data[dst_pos]][lane]) {
						result_data[search_num] = iter; /* found at iter => iter = path length */
						lane_to_num[lane] = -1;         // mark inactive
						active--;
					}
				}
			}
		}

		// no changes anymore: any still active searches have no path
		for (int64_t lane = 0; lane < LANE_LIMIT; lane++) {
			int64_t search_num = lane_to_num[lane];
			if (search_num >= 0) { // active lane
				result_validity.SetInvalid(search_num);
				result_data[search_num] = (int64_t)-1; /* no path */
				lane_to_num[lane] = -1;                // mark inactive
			}
		}
	}
	sqlpgq_state->csr_to_delete.insert(info.csr_id);
}

CreateScalarFunctionInfo DuckPGQFunctions::GetIterativeLengthFunction() {
	auto fun = ScalarFunction(
	    "iterativelength", {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
	    LogicalType::BIGINT, IterativeLengthFunction, IterativeLengthFunctionData::IterativeLengthBind);
	return CreateScalarFunctionInfo(fun);
}

} // namespace duckdb
