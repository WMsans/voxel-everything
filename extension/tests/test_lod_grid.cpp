#include <doctest/doctest.h>
#include "lod/lod_grid.h"
#include <algorithm>
#include <cmath>
#include <vector>

// Spec section 2's table, pinned. If these drift the mesher silently samples the wrong
// lattice and every downstream number (memory, range, page counts) is wrong with it.
TEST_CASE("the level table matches spec section 2") {
	CHECK(ve::kLodLevels == 8);
	CHECK(ve::kLodChunkCells == 32);
	CHECK(ve::kLodChunkLattice == 34);
	CHECK(ve::kLodChunkMeshCells == 33);
	CHECK(ve::kLodFineLattice == 69);
	CHECK(ve::lod_cell_size(0) == doctest::Approx(0.4f));
	CHECK(ve::lod_cell_size(1) == doctest::Approx(0.8f));
	CHECK(ve::lod_cell_size(4) == doctest::Approx(6.4f));
	CHECK(ve::lod_cell_size(7) == doctest::Approx(51.2f));
	CHECK(ve::lod_chunk_size(0) == doctest::Approx(12.8f));
	CHECK(ve::lod_chunk_size(7) == doctest::Approx(1638.4f));
	// Every level is exactly twice the one below.
	for (int l = 1; l < ve::kLodLevels; l++)
		CHECK(ve::lod_cell_size(l) == doctest::Approx(2.0f * ve::lod_cell_size(l - 1)));
}

// The descend threshold is stated once, in terms of the per-cell pixel error, so section 2's
// distance column and section 6.1's walk can never disagree about what "3 px" means.
TEST_CASE("the descend threshold is the per-cell error squared over a chunk") {
	CHECK(ve::kLodTargetCellPx == doctest::Approx(3.0f));
	CHECK(ve::kLodSseAreaThresh ==
			doctest::Approx(float(ve::kLodChunkCells) * ve::kLodTargetCellPx *
					float(ve::kLodChunkCells) * ve::kLodTargetCellPx));
	CHECK(ve::kLodSseAreaThresh == doctest::Approx(9216.0f));
}

// Chunk coordinates are GLOBAL per level: no origin term anywhere.
TEST_CASE("chunk coordinates are global and floor on negatives") {
	CHECK(ve::lod_chunk_of_point(0, 0.0f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_chunk_of_point(0, 12.79f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_chunk_of_point(0, 12.81f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	CHECK(ve::lod_chunk_of_point(0, -0.01f, 0.0f, 0.0f) == ve::IVec3{-1, 0, 0});
	CHECK(ve::lod_chunk_of_point(2, 51.1f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_chunk_of_point(2, 51.3f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	float o[3];
	ve::lod_chunk_origin(3, {2, -1, 0}, o);
	CHECK(o[0] == doctest::Approx(2.0f * 102.4f));
	CHECK(o[1] == doctest::Approx(-102.4f));
	CHECK(o[2] == doctest::Approx(0.0f));
}

// A parent's eight children tile it exactly, and every child maps back to that parent.
TEST_CASE("parent and child tile each other exactly") {
	const ve::IVec3 p{3, -2, 5};
	const ve::IVec3 base = ve::lod_child_base(p);
	CHECK(base == ve::IVec3{6, -4, 10});
	for (int k = 0; k < 8; k++) {
		const ve::IVec3 c{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
		CHECK(ve::lod_parent(c) == p);
	}
	CHECK(ve::lod_parent({-1, -1, -1}) == ve::IVec3{-1, -1, -1});
	CHECK(ve::lod_parent({-2, -2, -2}) == ve::IVec3{-1, -1, -1});
}

TEST_CASE("chunk distance is zero inside and grows outside") {
	const float c0[3] = {1.0f, 1.0f, 1.0f};
	CHECK(ve::lod_chunk_distance(0, {0, 0, 0}, c0) == doctest::Approx(0.0f));
	const float c1[3] = {-10.0f, 1.0f, 1.0f};
	CHECK(ve::lod_chunk_distance(0, {0, 0, 0}, c1) == doctest::Approx(10.0f));
	// The far distance is to the FARTHEST corner: it is what decides "entirely inside the
	// near field", where building the chunk would burn pages to draw nothing.
	const float o[3] = {0.0f, 0.0f, 0.0f};
	CHECK(ve::lod_chunk_far_distance(0, {0, 0, 0}, o) ==
			doctest::Approx(std::sqrt(3.0f) * 12.8f));
}

// An op must dirty every chunk whose stored quads it can move. Brute force over a
// neighbourhood: any chunk holding a lattice sample the op's AABB reaches must be inside
// the reported range.
TEST_CASE("op_lod_chunk_range covers every chunk the op can change") {
	ve::EditOp op;
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 20.0f; op.pos[1] = 51.0f; op.pos[2] = -3.0f;
	op.radius = 4.0f;
	for (int level = 0; level < ve::kLodLevels; level++) {
		ve::IVec3 lo{}, hi{};
		ve::op_lod_chunk_range(op, level, &lo, &hi);
		float olo[3], ohi[3];
		ve::op_world_aabb(op, olo, ohi);
		const float cell = ve::lod_cell_size(level);
		// Every sample position within one cell of the op's AABB belongs to a chunk in range.
		for (float z = olo[2] - cell; z <= ohi[2] + cell; z += cell * 0.5f)
			for (float y = olo[1] - cell; y <= ohi[1] + cell; y += cell * 0.5f)
				for (float x = olo[0] - cell; x <= ohi[0] + cell; x += cell * 0.5f) {
					const ve::IVec3 c = ve::lod_chunk_of_point(level, x, y, z);
					CHECK(c.x >= lo.x); CHECK(c.x <= hi.x);
					CHECK(c.y >= lo.y); CHECK(c.y <= hi.y);
					CHECK(c.z >= lo.z); CHECK(c.z <= hi.z);
				}
	}
}

TEST_CASE("root selection covers the camera's own root") {
	const float cam[3] = {8.0f, 62.0f, 8.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &roots);
	const ve::IVec3 own = ve::lod_chunk_of_point(ve::kLodLevels - 1, cam[0], cam[1], cam[2]);
	CHECK(std::find(roots.begin(), roots.end(), own) != roots.end());
}

TEST_CASE("root count grows with the radius and stays bounded") {
	const float cam[3] = {0.0f, 0.0f, 0.0f};
	std::vector<ve::IVec3> near_roots, far_roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &near_roots);
	ve::lod_roots_in_radius(cam, 4000.0f, &far_roots);
	CHECK(near_roots.size() >= 8);
	CHECK(near_roots.size() <= 27);   // (ceil(2R/1638.4) + 1)^3 candidates
	CHECK(far_roots.size() > near_roots.size());
	CHECK(far_roots.size() <= 216);
}

TEST_CASE("every emitted root actually intersects the radius") {
	const float cam[3] = {1234.0f, -56.0f, 7890.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 2000.0f, &roots);
	REQUIRE(!roots.empty());
	for (const ve::IVec3 &c : roots)
		CHECK(ve::lod_chunk_distance(ve::kLodLevels - 1, c, cam) <= 2000.0f);
}

TEST_CASE("root selection has no origin bias") {
	// The old world box put its origin at (0, -51.2, 0). A radius has no such anchor: the
	// same camera-relative geometry must appear 50 km away. Exact equality requires the
	// same intra-chunk position at both cameras (mid-chunk vs corner geometry differs
	// under any correct implementation); the aligned comparison is what tests
	// origin-independence.
	const float near_cam[3] = {0.0f, 0.0f, 0.0f};
	const float far_cam[3] = {49152.0f, 0.0f, 49152.0f};  // 30 x 1638.4 m = chunk corner
	std::vector<ve::IVec3> a, b;
	ve::lod_roots_in_radius(near_cam, 2000.0f, &a);
	ve::lod_roots_in_radius(far_cam, 2000.0f, &b);
	CHECK(a.size() == b.size());
}
