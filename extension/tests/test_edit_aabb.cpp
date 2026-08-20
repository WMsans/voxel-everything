#include <doctest/doctest.h>
#include "render/edit_aabb.h"
#include <algorithm>
#include <vector>

namespace {

ve::EditOp sphere(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereAdd;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

int64_t volume(const ve::EditAabb &a) {
	return static_cast<int64_t>(a.hi.x - a.lo.x + 1) *
			static_cast<int64_t>(a.hi.y - a.lo.y + 1) *
			static_cast<int64_t>(a.hi.z - a.lo.z + 1);
}

// The exact per-op brick range used by exact_edit_aabbs, clamped to the region.
ve::EditAabb input_range(const ve::IVec3 &region, const ve::EditOp &op) {
	ve::IVec3 lo{}, hi{};
	ve::op_brick_range(op, &lo, &hi);
	const ve::IVec3 r0{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	const ve::IVec3 r1{r0.x + ve::kRegionBricks - 1, r0.y + ve::kRegionBricks - 1,
			r0.z + ve::kRegionBricks - 1};
	lo.x = std::max(lo.x, r0.x);
	lo.y = std::max(lo.y, r0.y);
	lo.z = std::max(lo.z, r0.z);
	hi.x = std::min(hi.x, r1.x);
	hi.y = std::min(hi.y, r1.y);
	hi.z = std::min(hi.z, r1.z);
	return {lo, hi};
}

bool covers(const ve::EditAabb &outer, const ve::EditAabb &inner) {
	return outer.lo.x <= inner.lo.x && outer.hi.x >= inner.hi.x &&
			outer.lo.y <= inner.lo.y && outer.hi.y >= inner.hi.y &&
			outer.lo.z <= inner.lo.z && outer.hi.z >= inner.hi.z;
}

} // namespace

TEST_CASE("scattered exact edit clusters stay disjoint instead of one union AABB") {
	// Region (1, 3, 0) covers global bricks 32..63 x 96..127 x 0..31, i.e. world metres
	// 25.6..51.2 x 76.8..102.4 x 0..25.6. Four clusters far apart in that one region each
	// pile up overlapping ops; the old union AABB would span nearly the whole region.
	const ve::IVec3 region{1, 3, 0};
	const float clusters[4][3] = {
		{30.0f, 80.0f, 2.0f},
		{48.0f, 80.0f, 2.0f},
		{30.0f, 100.0f, 24.0f},
		{48.0f, 100.0f, 24.0f},
	};
	std::vector<ve::EditOp> ops;
	for (const auto &c : clusters) {
		for (int i = 0; i < 40; i++) {
			ops.push_back(sphere(c[0], c[1] + i * 0.05f, c[2], 1.5f));
		}
	}

	std::vector<ve::EditAabb> boxes;
	REQUIRE(ve::exact_edit_aabbs(region, ops, &boxes));
	CHECK(boxes.size() == 4);
	int64_t total = 0;
	for (const auto &b : boxes) {
		// Each cluster stays a small box, not a region-covering union.
		CHECK(volume(b) < 4096);
		total += volume(b);
	}
	CHECK(total < 4096);
}

TEST_CASE("overlapping exact edit ops merge into one bounded AABB") {
	const ve::IVec3 region{0, 3, 0};
	std::vector<ve::EditOp> ops;
	for (int i = 0; i < 80; i++) {
		ops.push_back(sphere(20.0f, 90.0f + i * 0.05f, 20.0f, 1.5f));
	}
	std::vector<ve::EditAabb> boxes;
	REQUIRE(ve::exact_edit_aabbs(region, ops, &boxes));
	CHECK(boxes.size() == 1);
	CHECK(volume(boxes[0]) < 4096);
}

TEST_CASE("exact-edit AABB cap splits an overlapping chain across a region") {
	// Forty small spheres along the region diagonal touch/overlap in a transitive chain.
	// Without the cap the merged AABB spans nearly the whole 32^3 region; with the cap the
	// result must be several bounded AABBs that still cover every op's own brick range.
	const ve::IVec3 region{0, 0, 0};
	const int steps = 40;
	std::vector<ve::EditOp> ops;
	for (int i = 0; i < steps; i++) {
		const float t = static_cast<float>(i) / static_cast<float>(steps - 1);
		const float p = 0.5f + t * (31.0f * 0.8f);
		ops.push_back(sphere(p, p, p, 1.0f));
	}

	std::vector<ve::EditAabb> boxes;
	REQUIRE(ve::exact_edit_aabbs(region, ops, &boxes));
	CHECK(boxes.size() > 1);
	for (const auto &b : boxes) {
		CHECK(ve::aabb_volume(b) <= ve::kMaxExactEditAabbBricks);
	}
	for (const ve::EditOp &op : ops) {
		const ve::EditAabb range = input_range(region, op);
		bool covered = false;
		for (const auto &b : boxes) {
			if (covers(b, range)) {
				covered = true;
				break;
			}
		}
		CHECK(covered);
	}
}

TEST_CASE("empty edit log has no exact edit AABB") {
	const ve::IVec3 region{0, 3, 0};
	std::vector<ve::EditOp> ops;
	std::vector<ve::EditAabb> boxes;
	CHECK_FALSE(ve::exact_edit_aabbs(region, ops, &boxes));
	CHECK(boxes.empty());
}
