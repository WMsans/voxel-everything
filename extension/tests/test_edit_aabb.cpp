#include <doctest/doctest.h>
#include "render/edit_aabb.h"
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

TEST_CASE("empty edit log has no exact edit AABB") {
	const ve::IVec3 region{0, 3, 0};
	std::vector<ve::EditOp> ops;
	std::vector<ve::EditAabb> boxes;
	CHECK_FALSE(ve::exact_edit_aabbs(region, ops, &boxes));
	CHECK(boxes.empty());
}
