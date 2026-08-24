#include <doctest/doctest.h>
#include "world/brick.h"
#include "world/brick_clamp.h"
#include "generator/edit_ops.h"
#include <cmath>
#include <vector>

namespace {

// Largest |d(neighbour) - d(sample)| over every axis-aligned pair in the lattice, in metres.
// A field a sphere tracer can safely step by has this at or below one voxel pitch.
float worst_step(const uint8_t *sdf) {
	float worst = 0.0f;
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const float d = ve::decode_sdf(sdf[ve::sdf_index(x, y, z)]);
				const int nb[3][3] = {{x + 1, y, z}, {x, y + 1, z}, {x, y, z + 1}};
				for (auto &n : nb) {
					if (n[0] >= ve::kBrickSdfStride || n[1] >= ve::kBrickSdfStride ||
							n[2] >= ve::kBrickSdfStride) continue;
					const float e = ve::decode_sdf(sdf[ve::sdf_index(n[0], n[1], n[2])]);
					worst = std::max(worst, std::fabs(e - d));
				}
			}
	return worst;
}

} // namespace

TEST_CASE("the clamp makes a discontinuous lattice 1-Lipschitz") {
	std::vector<uint8_t> sdf(ve::kBrickSdfCount);
	// A step discontinuity exactly like the one a hardness seam produces: solid on one
	// side of the x midplane, far-air on the other, with nothing in between.
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++)
				sdf[ve::sdf_index(x, y, z)] = ve::encode_sdf(x < 8 ? -0.6f : 0.6f);

	CHECK(worst_step(sdf.data()) > ve::kVoxelSize * 4.0f); // the bug, before
	ve::clamp_brick_lattice(sdf.data());
	// One quantisation step of slack: the lattice is uint8 and the clamp works in that
	// space, so an exact <= kVoxelSize would be a test of rounding, not of the algorithm.
	const float slack = 2.0f * ve::kSdfRange / 255.0f;
	CHECK(worst_step(sdf.data()) <= ve::kVoxelSize + slack);
}

TEST_CASE("the clamp never flips a sample's sign") {
	std::vector<uint8_t> before(ve::kBrickSdfCount), after(ve::kBrickSdfCount);
	for (int i = 0; i < ve::kBrickSdfCount; i++)
		before[i] = static_cast<uint8_t>((i * 37) % 256); // arbitrary but reproducible
	after = before;
	ve::clamp_brick_lattice(after.data());
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		const float a = ve::decode_sdf(before[i]), b = ve::decode_sdf(after[i]);
		CHECK_MESSAGE((a > 0.0f) == (b > 0.0f), "clamp flipped a sign at " << i);
		CHECK(std::fabs(b) <= std::fabs(a) + 1e-6f); // magnitudes only ever shrink
	}
}

TEST_CASE("the clamp is idempotent") {
	std::vector<uint8_t> a(ve::kBrickSdfCount);
	for (int i = 0; i < ve::kBrickSdfCount; i++) a[i] = static_cast<uint8_t>((i * 91) % 256);
	ve::clamp_brick_lattice(a.data());
	std::vector<uint8_t> b = a;
	ve::clamp_brick_lattice(b.data());
	// Converged means converged. If this fails the iteration count is too low, and the GPU
	// mirror in Task 8 will not be able to agree with this side.
	CHECK(a == b);
}

TEST_CASE("the gate is off until two materials of differing hardness meet") {
	ve::EditOp carve{};
	carve.type = ve::kOpSphereSubtract;
	carve.radius = 1.0f;
	const uint16_t one_material[4] = {1, 1, 1, 1};
	const uint16_t two_same[4] = {0, 0, 0, 0};   // air only
	CHECK_FALSE(ve::lattice_needs_clamp(one_material, 4, &carve, 1));
	CHECK_FALSE(ve::lattice_needs_clamp(two_same, 4, &carve, 1));

	// grass (1.0) beside rock (3.0) with a carve present: this is the case that distorts.
	const uint16_t mixed[4] = {1, 2, 1, 2};
	CHECK(ve::lattice_needs_clamp(mixed, 4, &carve, 1));

	// Same materials, no subtract op: nothing distorts the field, so no clamp.
	ve::EditOp paint{};
	paint.type = ve::kOpSpherePaint;
	CHECK_FALSE(ve::lattice_needs_clamp(mixed, 4, &paint, 1));
}
