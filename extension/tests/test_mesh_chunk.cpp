#include <doctest/doctest.h>
#include "mesh/mesh_chunk.h"
#include "generator/generator.h"
#include "world/brick_eval.h" // ve::eval_field, for the brute-force oracle below
#include <cmath>
#include <vector>

// A chunk must lie inside exactly one region: that is the whole reason the mesher can
// reconstruct it from a single op list (EditLog appends an op to every region it touches).
TEST_CASE("a chunk is 8 bricks and never straddles a region border") {
	CHECK(ve::kChunkBricks == 8);
	CHECK(ve::kChunkSize == doctest::Approx(6.4f));
	CHECK(ve::kChunkCells == 64);
	// The sampling PITCH is the collision fidelity, and it did not change when the chunk
	// edge halved — only how much surface one Jolt shape carries.
	CHECK(ve::kChunkCellSize == doctest::Approx(0.1f));
	CHECK(ve::kChunkMeshCells == 65);
	CHECK(ve::kChunkLattice == 66);
	CHECK(ve::kRegionBricks % ve::kChunkBricks == 0);
	for (int cz = -3; cz < 3; cz++)
		for (int cy = -3; cy < 3; cy++)
			for (int cx = -3; cx < 3; cx++) {
				const ve::IVec3 c{cx, cy, cz};
				const ve::IVec3 r = ve::region_of_chunk(c);
				float o[3];
				ve::chunk_world_origin(c, o);
				for (int k = 0; k < 8; k++) {
					const float e = 0.01f, s = ve::kChunkSize - 0.01f;
					const float p[3] = {o[0] + ((k & 1) ? s : e), o[1] + ((k & 2) ? s : e),
							o[2] + ((k & 4) ? s : e)};
					CHECK(ve::WorldBounds::region_of_point(p[0], p[1], p[2]) == r);
				}
			}
}

TEST_CASE("chunk lookups floor on negative coordinates") {
	CHECK(ve::chunk_of_point(0.0f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::chunk_of_point(6.39f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::chunk_of_point(6.41f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	CHECK(ve::chunk_of_point(-0.01f, 0.0f, 0.0f) == ve::IVec3{-1, 0, 0});
	CHECK(ve::chunk_of_brick({-1, 0, 7}) == ve::IVec3{-1, 0, 0});
	CHECK(ve::chunk_of_brick({8, -8, 23}) == ve::IVec3{1, -1, 2});
	CHECK(ve::chunk_min_brick({2, -1, 0}) == ve::IVec3{16, -8, 0});
}

TEST_CASE("chunk_distance is zero inside and grows outside") {
	CHECK(ve::chunk_distance({0, 0, 0}, 1.0f, 1.0f, 1.0f) == doctest::Approx(0.0f));
	CHECK(ve::chunk_distance({0, 0, 0}, -10.0f, 1.0f, 1.0f) == doctest::Approx(10.0f));
	CHECK(ve::chunk_distance({1, 0, 0}, 0.0f, 0.0f, 0.0f) == doctest::Approx(ve::kChunkSize));
}

// An op must dirty every chunk whose STORED TRIANGLES it can move. Brute force: walk the
// chunks around the op and check that any chunk holding a lattice sample the op changes is
// inside the reported range.
TEST_CASE("op_chunk_range covers every chunk whose lattice the op changes") {
	ve::EditOp op;
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 20.0f; op.pos[1] = 51.0f; op.pos[2] = -3.0f;
	op.radius = 4.0f;
	ve::IVec3 lo{}, hi{};
	ve::op_chunk_range(op, &lo, &hi);

	const ve::IVec3 c0 = ve::chunk_of_point(op.pos[0], op.pos[1], op.pos[2]);
	for (int dz = -3; dz <= 3; dz++)
		for (int dy = -3; dy <= 3; dy++)
			for (int dx = -3; dx <= 3; dx++) {
				const ve::IVec3 c{c0.x + dx, c0.y + dy, c0.z + dz};
				float o[3];
				ve::chunk_world_origin(c, o);
				// Does any lattice sample of this chunk fall inside the op's sphere? Sample
				// the lattice coarsely (every 8th) plus its exact extremes: the sphere is
				// far wider than 8 cells, so nothing can hide between the probes.
				bool touched = false;
				for (int i = -1; i <= ve::kChunkCells && !touched; i += 8)
					for (int j = -1; j <= ve::kChunkCells && !touched; j += 8)
						for (int k = -1; k <= ve::kChunkCells && !touched; k += 8) {
							const float p[3] = {o[0] + i * ve::kChunkCellSize,
									o[1] + j * ve::kChunkCellSize, o[2] + k * ve::kChunkCellSize};
							const float d = std::sqrt((p[0] - op.pos[0]) * (p[0] - op.pos[0]) +
									(p[1] - op.pos[1]) * (p[1] - op.pos[1]) +
									(p[2] - op.pos[2]) * (p[2] - op.pos[2]));
							touched = d <= op.radius;
						}
				if (!touched) continue;
				CHECK(c.x >= lo.x); CHECK(c.y >= lo.y); CHECK(c.z >= lo.z);
				CHECK(c.x <= hi.x); CHECK(c.y <= hi.y); CHECK(c.z <= hi.z);
			}
}

// The probe may say "maybe" about an empty chunk (it only costs a wasted mesh job), but it
// may never say "no" about a chunk the mesher would find a surface in.
TEST_CASE("chunk_has_surface never misses a chunk that holds a zero crossing") {
	const ve::AnalyticGenerator gen;
	int surfaced = 0;
	// The generator's terrain sits at ~51.2 m; sweep the chunk layers straddling it. Derived
	// from kChunkSize so the window follows the chunk, rather than a literal layer number that
	// silently slid off the surface when the chunk edge changed.
	const int surf = static_cast<int>(51.2f / ve::kChunkSize);
	for (int cz = -1; cz <= 2; cz++)
		for (int cy = surf - 2; cy <= surf + 2; cy++)
			for (int cx = -1; cx <= 2; cx++) {
				const ve::IVec3 c{cx, cy, cz};
				float o[3];
				ve::chunk_world_origin(c, o);
				bool pos = false, neg = false;
				for (int i = 0; i <= 32; i++)
					for (int j = 0; j <= 32; j++)
						for (int k = 0; k <= 32; k++) {
							const float s = ve::eval_field(gen, nullptr, 0,
									o[0] + i * (ve::kChunkSize / 32.0f),
									o[1] + j * (ve::kChunkSize / 32.0f),
									o[2] + k * (ve::kChunkSize / 32.0f)).sdf;
							if (s <= 0.0f) neg = true; else pos = true;
						}
				if (neg && pos) {
					surfaced++;
					CHECK(ve::chunk_has_surface(gen, nullptr, 0, c));
				}
			}
	CHECK(surfaced > 4); // the sweep really did cross the surface
}

TEST_CASE("chunk_has_surface rejects open sky and deep rock") {
	const ve::AnalyticGenerator gen;
	CHECK_FALSE(ve::chunk_has_surface(gen, nullptr, 0, {0, 12, 0}));  // y 153.6 .. 166.4
	CHECK_FALSE(ve::chunk_has_surface(gen, nullptr, 0, {0, -2, 0}));  // y -25.6 .. -12.8
}

// A sphere-add in open sky creates surface where the generator has none, so the op list has
// to be part of the verdict.
TEST_CASE("chunk_has_surface sees ops, not just the generator") {
	const ve::AnalyticGenerator gen;
	const ve::IVec3 sky{0, 12, 0};
	CHECK_FALSE(ve::chunk_has_surface(gen, nullptr, 0, sky));
	ve::EditOp add;
	add.type = ve::kOpSphereAdd;
	add.material = 4;
	add.pos[0] = 6.4f; add.pos[1] = 12 * ve::kChunkSize + 6.4f; add.pos[2] = 6.4f;
	add.radius = 3.0f;
	CHECK(ve::chunk_has_surface(gen, &add, 1, sky));
}
