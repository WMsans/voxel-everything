#include "core/edit_pipeline.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace ve {

Invalidation Invalidation::consolidated(IVec3 region) {
	Invalidation inv;
	inv.reason = InvalidationReason::kConsolidated;
	inv.region = region;
	const IVec3 base{region.x * kRegionBricks, region.y * kRegionBricks, region.z * kRegionBricks};
	float first_hi[3], last_lo[3];
	brick_world_aabb(base, inv.lo, first_hi);
	brick_world_aabb({base.x + kRegionBricks - 1, base.y + kRegionBricks - 1,
							 base.z + kRegionBricks - 1},
			last_lo, inv.hi);
	// A bake changes no field value, so nothing that was holding on can have been loosened.
	inv.notify_islands = false;
	return inv;
}

bool EditPipeline::preflight(std::span<const EditOp> ops, std::vector<IVec3> *full) const {
	if (full) full->clear();
	EditLog *log = log_ ? *log_ : nullptr;
	if (!log) return false;
	bool ok = true;
	std::map<std::tuple<int, int, int>, int> adds;
	for (const EditOp &op : ops) {
		// EditLog::append refuses these outright, so a batch holding one can never be
		// accepted whole. There is no region to name.
		if (!edit_op_is_well_formed(op) || !op_region_span_ok(op)) {
			ok = false;
			continue;
		}
		IVec3 lo{}, hi{};
		op_region_range(op, &lo, &hi);
		for (int z = lo.z; z <= hi.z; z++)
			for (int y = lo.y; y <= hi.y; y++)
				for (int x = lo.x; x <= hi.x; x++) adds[std::tuple<int, int, int>{x, y, z}]++;
	}
	for (const auto &entry : adds) {
		const IVec3 region{std::get<0>(entry.first), std::get<1>(entry.first),
				std::get<2>(entry.first)};
		if (log->op_count(region) + entry.second <= kMaxRegionOps) continue;
		ok = false;
		if (full) full->push_back(region);
	}
	return ok;
}

BatchResult EditPipeline::apply(std::span<const EditOp> ops, EditPolicy policy) {
	BatchResult out;
	out.ops.resize(ops.size());
	EditLog *log = log_ ? *log_ : nullptr;
	if (!log) return out;
	if (policy.atomic && !preflight(ops, &out.full)) {
		out.refused = true;
		return out;
	}
	for (size_t i = 0; i < ops.size(); i++) {
		const EditOp &op = ops[i];
		// A reference into a vector that was sized up front: it never reallocates, so the
		// pointer an Invalidation carries stays valid for the whole record() call.
		EditLog::AppendResult &r = out.ops[i];
		r = log->append(op);
		if (r.oversized || !r.rejected.empty()) {
			Invalidation inv;
			inv.reason = InvalidationReason::kRejected;
			inv.seq = seq_ ? seq_->load(std::memory_order_relaxed) : 0;
			inv.op = &op;
			inv.append = &r;
			inv.notify_islands = policy.notify_islands;
			record(inv);
		}
		// Empty results are fail-soft no-ops: malformed/oversized and fully rejected ops
		// changed no field state, so they must not advance the edit sequence, wake
		// connectivity, or enter the render-thread pending queue.
		if (r.touched.empty()) continue;
		// Bump AFTER the append and under the same lock the streamer uses to capture op
		// counts. If the seq moved before the append, a readback stamped between the bump and
		// the append would claim edits that are not in the GPU state it describes.
		if (seq_) seq_->fetch_add(1, std::memory_order_relaxed);
		Invalidation inv;
		inv.reason = InvalidationReason::kEdit;
		op_world_aabb(op, inv.lo, inv.hi);
		inv.seq = seq_ ? seq_->load(std::memory_order_relaxed) : 0;
		inv.op = &op;
		inv.append = &r;
		inv.notify_islands = policy.notify_islands;
		record(inv);
	}
	return out;
}

void EditPipeline::invalidate(const Invalidation &inv) { record(inv); }

void EditPipeline::add_sink(InvalidationSink *sink) {
	if (!sink) return;
	if (std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end()) return;
	sinks_.push_back(sink);
}

void EditPipeline::remove_sink(InvalidationSink *sink) {
	sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void EditPipeline::record(const Invalidation &inv) {
	for (InvalidationSink *sink : sinks_) sink->record(inv);
}

} // namespace ve
