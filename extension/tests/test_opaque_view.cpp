#include <doctest/doctest.h>
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/material_table.h"
#include "world/opaque_view.h"
#include "analytic_oracle.h"

TEST_CASE("opaque_view turns a transparent solid into just-outside and nothing else") {
	const uint16_t ice = ve::material_id("ice");
	const uint16_t rock = ve::material_id("rock");
	float d = -0.3f;
	uint16_t m = ice;
	ve::opaque_view(&d, &m);
	CHECK(d == doctest::Approx(ve::kOpaqueOutside));
	CHECK(d > 0.0f);
	CHECK(m == 0);

	d = -0.3f; m = rock;
	ve::opaque_view(&d, &m);
	CHECK(d == doctest::Approx(-0.3f));
	CHECK(m == rock);

	// Air that merely carries a transparent label (a projected material) is untouched.
	d = 0.2f; m = ice;
	ve::opaque_view(&d, &m);
	CHECK(d == doctest::Approx(0.2f));
	CHECK(m == ice);
}

namespace {

// The brick holding the default world's surface under (20, *, 30): the first brick, walking
// down from y = 80 m, that the union probe calls a surface brick.
ve::IVec3 surface_brick(const ve::Generator &gen) {
	for (int by = 100; by > 0; by--) {
		const ve::IVec3 b{25, by, 37};
		if (ve::brick_has_surface(gen, nullptr, 0, b, nullptr, nullptr, false)) return b;
	}
	return ve::IVec3{25, 64, 37};
}

ve::EditOp sphere(uint32_t type, uint16_t material, const float c[3], float r) {
	ve::EditOp op{};
	op.type = type;
	op.material = material;
	op.pos[0] = c[0]; op.pos[1] = c[1]; op.pos[2] = c[2];
	op.radius = r;
	return op;
}

} // namespace

TEST_CASE("a brick buried in an ice ball has no opaque surface but is still solid for occupancy") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 b = surface_brick(gen);
	float bo[3];
	ve::brick_world_origin(b, bo);
	// A 3 m ice ball centred 2 m above the brick: the brick's air half is now ice.
	const float c[3] = {bo[0] + 0.4f, bo[1] + 2.0f, bo[2] + 0.4f};
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f);

	ve::BrickEval opaque{}, plain{};
	ve::eval_brick(gen, &add, 1, b, &opaque, nullptr, nullptr, true);
	ve::eval_brick(gen, &add, 1, b, &plain, nullptr, nullptr, false);
	const uint8_t zero = ve::encode_sdf(0.0f);
	int opaque_out = 0, union_out = 0;
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		if (opaque.brick.sdf[i] > zero) opaque_out++;
		if (plain.brick.sdf[i] > zero) union_out++;
	}
	CHECK(union_out == 0);   // the union lattice is solid through and through
	CHECK(opaque_out > 0);   // the opaque view still has the ground's own surface in it
	CHECK(ve::cell_state_field(gen, &add, 1, b) == ve::kCellFull); // occupancy: the UNION
	// Residency: the union probe alone would drop this brick; the opaque probe keeps it.
	CHECK_FALSE(ve::brick_has_surface(gen, &add, 1, b, nullptr, nullptr, false));
	CHECK(ve::brick_has_surface(gen, &add, 1, b, nullptr, nullptr, true));
}

TEST_CASE("with no transparent material the opaque view changes nothing") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 b = surface_brick(gen);
	ve::BrickEval opaque{}, plain{};
	ve::eval_brick(gen, nullptr, 0, b, &opaque, nullptr, nullptr, true);
	ve::eval_brick(gen, nullptr, 0, b, &plain, nullptr, nullptr, false);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(opaque.brick.sdf[i] == plain.brick.sdf[i]);
	// Brick::mat is the PACKED 2-bit index array, not one byte per cell.
	for (size_t i = 0; i < sizeof(opaque.brick.mat); i++) CHECK(opaque.brick.mat[i] == plain.brick.mat[i]);
	for (int k = 0; k < ve::kBrickPaletteSize; k++) CHECK(opaque.brick.palette[k] == plain.brick.palette[k]);
}

TEST_CASE("no palette entry of an opaque-view brick is a transparent material") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 b = surface_brick(gen);
	float bo[3];
	ve::brick_world_origin(b, bo);
	const float c[3] = {bo[0] + 0.4f, bo[1] + 0.6f, bo[2] + 0.4f};
	const ve::EditOp paint = sphere(ve::kOpSpherePaint, ve::material_id("ice"), c, 0.5f);
	ve::BrickEval e{};
	ve::eval_brick(gen, &paint, 1, b, &e, nullptr, nullptr, true);
	for (int k = 0; k < ve::kBrickPaletteSize; k++)
		CHECK_FALSE(ve::material_transparent(e.brick.palette[k]));
}
