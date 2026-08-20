#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace ve {

// A single axis-aligned brick range (inclusive, global brick coordinates).
struct EditAabb {
	IVec3 lo{};
	IVec3 hi{};
};

// Disjoint AABBs covering the overlapping parts of a region's edit log, clamped to that
// region. Overlapping ops are merged so recovery/repair re-marks do not duplicate work;
// separated clusters remain separate small boxes instead of growing into one
// region-covering union.
inline bool exact_edit_aabbs(const IVec3 &region, const std::vector<EditOp> &ops,
		std::vector<EditAabb> *out) {
	out->clear();
	const IVec3 r0{region.x * kRegionBricks, region.y * kRegionBricks,
			region.z * kRegionBricks};
	const IVec3 r1{r0.x + kRegionBricks - 1, r0.y + kRegionBricks - 1,
			r0.z + kRegionBricks - 1};
	std::vector<EditAabb> boxes;
	for (const EditOp &op : ops) {
		IVec3 blo{}, bhi{};
		op_brick_range(op, &blo, &bhi);
		blo.x = std::max(blo.x, r0.x);
		blo.y = std::max(blo.y, r0.y);
		blo.z = std::max(blo.z, r0.z);
		bhi.x = std::min(bhi.x, r1.x);
		bhi.y = std::min(bhi.y, r1.y);
		bhi.z = std::min(bhi.z, r1.z);
		if (blo.x > bhi.x || blo.y > bhi.y || blo.z > bhi.z) continue;
		boxes.push_back({blo, bhi});
	}

	// Merge only boxes that actually overlap. Touching-but-not-overlapping boxes are left
	// separate so a chain of edits cannot turn into one region-covering union through
	// adjacency alone.
	bool merged = true;
	while (merged) {
		merged = false;
		for (size_t i = 0; i < boxes.size() && !merged; i++) {
			for (size_t j = i + 1; j < boxes.size() && !merged; j++) {
				const EditAabb &a = boxes[i];
				const EditAabb &b = boxes[j];
				const bool overlap = a.lo.x <= b.hi.x && a.hi.x >= b.lo.x &&
						a.lo.y <= b.hi.y && a.hi.y >= b.lo.y &&
						a.lo.z <= b.hi.z && a.hi.z >= b.lo.z;
				if (!overlap) continue;
				boxes[i].lo.x = std::min(a.lo.x, b.lo.x);
				boxes[i].lo.y = std::min(a.lo.y, b.lo.y);
				boxes[i].lo.z = std::min(a.lo.z, b.lo.z);
				boxes[i].hi.x = std::max(a.hi.x, b.hi.x);
				boxes[i].hi.y = std::max(a.hi.y, b.hi.y);
				boxes[i].hi.z = std::max(a.hi.z, b.hi.z);
				boxes[j] = boxes.back();
				boxes.pop_back();
				merged = true;
			}
		}
	}

	*out = std::move(boxes);
	return !out->empty();
}

} // namespace ve
