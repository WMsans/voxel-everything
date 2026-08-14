#include "world/edit_log.h"

namespace ve {

namespace {
const std::vector<EditOp> kEmpty;
} // namespace

EditLog::AppendResult EditLog::append(const EditOp &op) {
	AppendResult result;
	IVec3 lo{}, hi{};
	op_region_range(op, &lo, &hi);
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 r{x, y, z};
				if (!bounds_.contains_region(r)) continue;
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
