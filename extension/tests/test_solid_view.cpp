#include <doctest/doctest.h>
#include "connectivity/contact_refine.h"
#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "world/brick_eval.h"
#include "world/material_table.h"
#include "world/opaque_view.h"
#include "analytic_oracle.h"
#include <algorithm>

// The SOLID VIEW (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3): the world as
// colliders, occupancy, contact refinement and island extraction see it. A liquid is air; a
// transparent solid that is not a liquid (ice) stays solid.

namespace {

ve::EditOp sphere(uint32_t type, uint16_t material, const float c[3], float r) {
	ve::EditOp op{};
	op.type = type;
	op.material = material;
	op.pos[0] = c[0]; op.pos[1] = c[1]; op.pos[2] = c[2];
	op.radius = r;
	return op;
}

// A point one metre under the default world's surface at (20, *, 30): walking down from
// 120 m, the first sample whose field is at most -1.
void buried_point(const ve::Generator &gen, float out[3]) {
	out[0] = 20.0f; out[1] = 0.0f; out[2] = 30.0f;
	for (float y = 120.0f; y > -50.0f; y -= 0.05f)
		if (gen.sample(20.0f, y, 30.0f).sdf <= -1.0f) { out[1] = y; return; }
}

// The brick test_opaque_view.cpp uses as "all air": 80 m up, far above the meadow.
constexpr ve::IVec3 kSkyBrick{25, 100, 37};

void brick_centre(ve::IVec3 b, float out[3]) {
	ve::brick_world_origin(b, out);
	for (int a = 0; a < 3; a++) out[a] += 0.5f * ve::kBrickSize;
}

} // namespace

TEST_CASE("solid_view turns a liquid solid into just-outside and leaves ice and rock alone") {
	const uint16_t water = ve::material_id("water");
	const uint16_t ice = ve::material_id("ice");
	const uint16_t rock = ve::material_id("rock");
	float d = -0.3f;
	uint16_t m = water;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(ve::kOpaqueOutside));
	CHECK(m == 0);
	d = -0.3f; m = ice;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(-0.3f));
	CHECK(m == ice);
	d = -0.3f; m = rock;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(-0.3f));
	CHECK(m == rock);
	// Air that merely carries a liquid label (a projected material) is untouched.
	d = 0.2f; m = water;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(0.2f));
	CHECK(m == water);
}

// The pit defect the transparency branch measured, for the solid view: carving the union's
// winner would delete the ground a water ball merely overlaps, and the player would fall
// into ground that the marcher still draws.
TEST_CASE("a water add contributes nothing to the solid view; the union still holds water") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("water"), q, 4.0f);
	const ve::Sample bare = ve::eval_field(gen, nullptr, 0, q[0], q[1], q[2]);
	const ve::Sample solid = ve::eval_field_solid(gen, &add, 1, q[0], q[1], q[2]);
	const ve::Sample uni = ve::eval_field(gen, &add, 1, q[0], q[1], q[2]);
	CHECK(solid.sdf == bare.sdf);
	CHECK(solid.material == bare.material);
	CHECK(uni.material == ve::material_id("water"));
	CHECK(uni.sdf < 0.0f);
}

TEST_CASE("ice stays solid in the solid view") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	q[1] += 3.0f; // above the ground, inside the ball: a sample only the ice makes solid
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("ice"), q, 2.0f);
	const ve::Sample solid = ve::eval_field_solid(gen, &add, 1, q[0], q[1], q[2]);
	const ve::Sample uni = ve::eval_field(gen, &add, 1, q[0], q[1], q[2]);
	CHECK(solid.sdf == uni.sdf);
	CHECK(solid.material == ve::material_id("ice"));
	CHECK(solid.sdf < 0.0f);
}

// Review Focus 2: painted water relabels ground in place and must not stop it colliding.
TEST_CASE("water paint on the ground leaves the solid view untouched") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	const ve::EditOp paint = sphere(ve::kOpSpherePaint, ve::material_id("water"), q, 2.0f);
	const ve::Sample bare = ve::eval_field(gen, nullptr, 0, q[0], q[1], q[2]);
	const ve::Sample solid = ve::eval_field_solid(gen, &paint, 1, q[0], q[1], q[2]);
	CHECK(solid.sdf == bare.sdf);
	CHECK(solid.material == bare.material);
	CHECK(ve::eval_field(gen, &paint, 1, q[0], q[1], q[2]).material == ve::material_id("water"));
}

TEST_CASE("with no liquid the solid view is the union, sample for sample") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	const float up[3] = {q[0], q[1] + 1.5f, q[2]};
	const ve::EditOp ops[3] = {
		sphere(ve::kOpSphereAdd, ve::material_id("rock"), up, 1.2f),
		sphere(ve::kOpSphereSubtract, 0, q, 0.8f),
		sphere(ve::kOpSphereAdd, ve::material_id("ice"), up, 0.6f),
	};
	for (int z = -4; z <= 4; z++)
		for (int y = -4; y <= 4; y++)
			for (int x = -4; x <= 4; x++) {
				const float p[3] = {q[0] + x * 0.4f, q[1] + y * 0.4f, q[2] + z * 0.4f};
				const ve::Sample u = ve::eval_field(gen, ops, 3, p[0], p[1], p[2]);
				const ve::Sample s = ve::eval_field_solid(gen, ops, 3, p[0], p[1], p[2]);
				CHECK(s.sdf == u.sdf);
				CHECK(s.material == u.material);
			}
}

TEST_CASE("a brick inside a floating water ball is air for occupancy; inside ice it is full") {
	ve::AnalyticGenerator gen;
	float c[3];
	brick_centre(kSkyBrick, c);
	const ve::EditOp water = sphere(ve::kOpSphereAdd, ve::material_id("water"), c, 3.0f);
	const ve::EditOp ice = sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f);
	CHECK(ve::cell_state_field(gen, &water, 1, kSkyBrick) == ve::kCellAir);
	CHECK(ve::cell_state_probe(gen, &water, 1, kSkyBrick) == ve::kCellAir);
	CHECK(ve::cell_state_field(gen, &ice, 1, kSkyBrick) == ve::kCellFull);
	CHECK(ve::cell_state_probe(gen, &ice, 1, kSkyBrick) == ve::kCellFull);
}

TEST_CASE("a face buried in water has no contact; buried in ice it is all contact") {
	ve::AnalyticGenerator gen;
	float c[3];
	brick_centre(kSkyBrick, c);
	c[0] += 0.5f * ve::kBrickSize; // centred on the +x face of the brick
	const ve::EditOp water = sphere(ve::kOpSphereAdd, ve::material_id("water"), c, 3.0f);
	const ve::EditOp ice = sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f);
	CHECK(ve::contact_samples_field(gen, &water, 1, kSkyBrick, 0, 9) == 0);
	CHECK(ve::contact_samples_field(gen, &ice, 1, kSkyBrick, 0, 9) == 81);
}

TEST_CASE("island extraction strips water and keeps the rock") {
	ve::AnalyticGenerator gen;
	float c[3];
	brick_centre(kSkyBrick, c);
	const float cap[3] = {c[0], c[1] + 1.0f, c[2]};
	const ve::EditOp rock_only[1] = {sphere(ve::kOpSphereAdd, ve::material_id("rock"), c, 1.0f)};
	const ve::EditOp with_water[2] = {rock_only[0],
			sphere(ve::kOpSphereAdd, ve::material_id("water"), cap, 1.0f)};
	const float voxel = 0.15f;
	const int dim = 32; // 4.8 m: covers the rock ball and the water cap above it
	const float origin[3] = {c[0] - 2.4f, c[1] - 2.4f, c[2] - 2.4f};
	const float box[6] = {origin[0], origin[1], origin[2],
			origin[0] + dim * voxel, origin[1] + dim * voxel, origin[2] + dim * voxel};
	ve::VolumeData a, b;
	ve::extract_island_volume(gen, rock_only, 1, nullptr, nullptr, origin, voxel, dim, box, 1, &a);
	ve::extract_island_volume(gen, with_water, 2, nullptr, nullptr, origin, voxel, dim, box, 1, &b);
	CHECK(a.solid_voxels > 0);
	CHECK(b.solid_voxels == a.solid_voxels);
	const uint8_t water = static_cast<uint8_t>(ve::material_id("water"));
	CHECK(std::count(b.mat.begin(), b.mat.end(), water) == 0);
}
