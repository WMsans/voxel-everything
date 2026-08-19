#include <doctest/doctest.h>
#include "generator/edit_ops.h"
#include "world/brick.h"
#include <cmath>
#include <vector>

namespace {

ve::EditOp sphere(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

} // namespace

TEST_CASE("an op that overlaps the box is kept") {
	const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.8f, 0.8f, 0.8f};
	CHECK(ve::op_touches_aabb(sphere(0.4f, 0.4f, 0.4f, 1.0f), lo, hi, 0.0f));
	CHECK(ve::op_touches_aabb(sphere(2.0f, 0.4f, 0.4f, 1.5f), lo, hi, 0.0f));
}

TEST_CASE("an op that clears the box by more than the pad is dropped") {
	const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.8f, 0.8f, 0.8f};
	CHECK_FALSE(ve::op_touches_aabb(sphere(20.0f, 0.4f, 0.4f, 1.0f), lo, hi, 0.2f));
}

TEST_CASE("the pad is what keeps a grazing op") {
	// A sphere whose surface sits exactly one pad away from the box face must be KEPT: the
	// stored field is a narrow band and the evaluator's own activation margin reaches that
	// far. Dropping it is the failure mode that puts a seam on a brick boundary.
	const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.8f, 0.8f, 0.8f};
	const ve::EditOp op = sphere(0.4f, 0.4f, 0.8f + 1.0f + 0.19f, 1.0f);
	CHECK(ve::op_touches_aabb(op, lo, hi, 0.2f));
	CHECK_FALSE(ve::op_touches_aabb(op, lo, hi, 0.0f));
}

TEST_CASE("filtering never changes an evaluated sample inside the box") {
	// The property the whole task rests on: for any point in the box, applying the filtered
	// list gives the same sample as applying all of them. Checked on a grid of points
	// against a mixed op list, because "conservative" is only a claim until it is measured.
	ve::AnalyticGenerator gen;
	std::vector<ve::EditOp> all;
	for (int i = 0; i < 40; i++) {
		const float a = static_cast<float>(i) * 0.7f;
		all.push_back(sphere(24.0f + std::cos(a) * static_cast<float>(i),
				51.0f + std::sin(a) * 3.0f, 24.0f + std::sin(a) * static_cast<float>(i),
				0.5f + 0.1f * static_cast<float>(i % 5)));
	}
	const float lo[3] = {24.0f, 51.2f, 24.0f};
	const float hi[3] = {24.8f, 52.0f, 24.8f};
	std::vector<ve::EditOp> kept;
	for (const ve::EditOp &op : all)
		if (ve::op_touches_aabb(op, lo, hi, ve::kActivationPad + ve::kVoxelSize)) kept.push_back(op);
	CHECK(kept.size() < all.size()); // the filter has to actually drop something

	for (int i = 0; i <= 4; i++)
		for (int j = 0; j <= 4; j++)
			for (int k = 0; k <= 4; k++) {
				const float x = lo[0] + (hi[0] - lo[0]) * static_cast<float>(i) / 4.0f;
				const float y = lo[1] + (hi[1] - lo[1]) * static_cast<float>(j) / 4.0f;
				const float z = lo[2] + (hi[2] - lo[2]) * static_cast<float>(k) / 4.0f;
				const ve::Sample full = ve::apply_ops(gen.sample(x, y, z), all.data(),
						static_cast<int>(all.size()), x, y, z);
				const ve::Sample filtered = ve::apply_ops(gen.sample(x, y, z), kept.data(),
						static_cast<int>(kept.size()), x, y, z);
				// Encoded storage clamps at +/-kSdfRange, so agreement is required only
				// where the value is representable -- which is exactly where it is used.
				const float a = full.sdf < -ve::kSdfRange ? -ve::kSdfRange : full.sdf;
				const float b = filtered.sdf < -ve::kSdfRange ? -ve::kSdfRange : filtered.sdf;
				CHECK(a == doctest::Approx(b).epsilon(0.0001));
				CHECK(full.material == filtered.material);
			}
}
