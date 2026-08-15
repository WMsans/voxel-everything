#include "world/edit_log.h"
#include <algorithm>

namespace ve {

namespace {
const std::vector<EditOp> kEmpty;
} // namespace

EditLog::AppendResult EditLog::append(const EditOp &op) {
	AppendResult result;
	IVec3 lo{}, hi{};
	op_region_range(op, &lo, &hi);
	// Clamp the op's region range to the world extent ONCE, before looping. The old loop
	// iterated the FULL range and tested contains_region per cell, so a hostile radius
	// (e.g. ~1e5 m) iterated ~4.8e14 cells — a multi-minute freeze. In-bounds ops are
	// unaffected: the clamp is the identity and the per-cell bounds test is dropped.
	const IVec3 o = bounds_.origin_regions();
	const IVec3 mn{o.x, o.y, o.z};
	const IVec3 mx{o.x + bounds_.size_regions.x - 1, o.y + bounds_.size_regions.y - 1,
			o.z + bounds_.size_regions.z - 1};
	lo.x = std::max(lo.x, mn.x); lo.y = std::max(lo.y, mn.y); lo.z = std::max(lo.z, mn.z);
	hi.x = std::min(hi.x, mx.x); hi.y = std::min(hi.y, mx.y); hi.z = std::min(hi.z, mx.z);
	if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z) return result; // entirely outside
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 r{x, y, z};
				std::vector<EditOp> &list = lists_[Key{x, y, z}];
				if (static_cast<int>(list.size()) >= kMaxRegionOps) {
					result.rejected.push_back(r);
					continue;
				}
				list.push_back(op);
				result.touched.push_back(r);
			}
	return result;
}

const std::vector<EditOp> &EditLog::ops(IVec3 region) const {
	const auto it = lists_.find(Key{region.x, region.y, region.z});
	return it == lists_.end() ? kEmpty : it->second;
}

} // namespace ve
