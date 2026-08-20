#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace ve {

// A single axis-aligned brick range (inclusive, global brick coordinates).
struct EditAabb {
	IVec3 lo{};
	IVec3 hi{};
};

// The largest exact-edit AABB we allow to grow to. 4096 bricks is 16^3, one eighth of a
// region's 32^3 bricks. It is far larger than normal per-edit ranges, yet small enough
// that a handful of exact re-mark ranges fit comfortably in the default max_brick_jobs
// (16384) job list. When merging two overlapping AABBs would exceed this cap the boxes are
// left separate, accepting some duplicate exact re-marks on shared bricks rather than
// letting a transitive chain grow into one region-covering AABB.
inline constexpr int kMaxExactEditAabbBricks = 4096;

inline int64_t aabb_volume(const EditAabb &a) {
	return static_cast<int64_t>(a.hi.x - a.lo.x + 1) *
			static_cast<int64_t>(a.hi.y - a.lo.y + 1) *
			static_cast<int64_t>(a.hi.z - a.lo.z + 1);
}

// Bounded AABBs covering the edit-affected parts of a region's edit log, clamped to that
// region. Overlapping ops are merged so recovery/repair re-marks do not duplicate work,
// but only while the merged AABB stays within kMaxExactEditAabbBricks. Chains whose
// transitive union would exceed the cap are cut into multiple bounded AABBs; those AABBs
// may overlap where a merge was rejected, which is intentional (a few duplicate exact
// re-marks are cheaper than re-queuing a whole 32^3 region).
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

	// Merge only boxes that actually overlap and whose union still fits the cap. Touching
	// boxes are still merged when cheap, but a chain that would grow past the cap is left
	// as multiple bounded AABBs instead of one region-covering union.
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
				const EditAabb merged_box{std::min(a.lo.x, b.lo.x),
						std::min(a.lo.y, b.lo.y),
						std::min(a.lo.z, b.lo.z),
						std::max(a.hi.x, b.hi.x),
						std::max(a.hi.y, b.hi.y),
						std::max(a.hi.z, b.hi.z)};
				if (aabb_volume(merged_box) > kMaxExactEditAabbBricks) continue;
				boxes[i] = merged_box;
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
