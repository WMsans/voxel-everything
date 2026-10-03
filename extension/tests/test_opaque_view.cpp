#include <doctest/doctest.h>
#include "generator/edit_ops.h"
#include <algorithm>
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

// The lattice sample of `b`'s bare union bake that lies deepest in the solid: a point the
// ground is unambiguously there, which is the point every "the ball swallowed the ground"
// assertion below is measured at.
struct SolidPoint {
	ve::IVec3 brick;
	int index;  // lattice coordinate of p inside brick
	float p[3];
	float depth;
};

SolidPoint deepest_solid(const ve::Generator &gen) {
	const ve::IVec3 b = surface_brick(gen);
	float bo[3];
	ve::brick_world_origin(b, bo);
	ve::BrickEval bare{};
	ve::eval_brick(gen, nullptr, 0, b, &bare, nullptr, nullptr, false);
	int best = 0;
	for (int i = 1; i < ve::kBrickSdfCount; i++)
		if (bare.brick.sdf[i] < bare.brick.sdf[best]) best = i;
	const int vx = best % ve::kBrickSdfStride;
	const int vy = (best / ve::kBrickSdfStride) % ve::kBrickSdfStride;
	const int vz = best / (ve::kBrickSdfStride * ve::kBrickSdfStride);
	SolidPoint out{b, best, {bo[0] + vx * ve::kVoxelSize, bo[1] + vy * ve::kVoxelSize,
			bo[2] + vz * ve::kVoxelSize}, ve::decode_sdf(bare.brick.sdf[best])};
	return out;
}

// Brick::mat is a PACKED 2-bit palette index array (see world/brick.h), not one byte per
// cell, so a material assertion has to go through it.
uint16_t cell_material(const ve::Brick &b, int index) {
	return b.palette[(b.mat[index >> 2] >> ((index & 3) * 2)) & 3];
}

int count_cells(const ve::Brick &b, uint16_t material) {
	int n = 0;
	for (int i = 0; i < ve::kBrickVoxelCount; i++)
		if (cell_material(b, i) == material) n++;
	return n;
}

} // namespace

// THE DEFECT. A transparent material is air, so a transparent ADD op must contribute NOTHING
// to the opaque bake -- not be carved away after it has already won the min over the whole
// op stack. Inside a 4 m ball its sdf beats the analytic ground's everywhere it reaches, so
// the carve deleted the ground the ball merely overlapped and left a pit.
//
// Every assertion here is against the EXISTING API, so on the pre-fix tree this is a wrong
// measured lattice rather than a build error.
TEST_CASE("a transparent add op contributes nothing to the opaque view") {
	ve::AnalyticGenerator gen;
	const SolidPoint q = deepest_solid(gen);
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("ice"), q.p, 4.0f);

	ve::BrickEval ice{}, bare{};
	ve::eval_brick(gen, &add, 1, q.brick, &ice, nullptr, nullptr, true);
	ve::eval_brick(gen, nullptr, 0, q.brick, &bare, nullptr, nullptr, true);

	// The pit, named. Carving the ice winner leaves the brick with no ground in it at all;
	// the ground's own depth must survive underneath.
	float deepest_ice = 1e30f, deepest_bare = 1e30f;
	int moved = 0;
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		deepest_ice = std::min(deepest_ice, ve::decode_sdf(ice.brick.sdf[i]));
		deepest_bare = std::min(deepest_bare, ve::decode_sdf(bare.brick.sdf[i]));
		if (ice.brick.sdf[i] != bare.brick.sdf[i]) moved++;
	}
	CHECK(deepest_ice == doctest::Approx(deepest_bare));
	CHECK(deepest_ice < 0.0f);   // the ground really is solid here, not just unmodified
	CHECK(moved == 0);          // and byte-identical, lattice and materials both
	for (size_t i = 0; i < sizeof(ice.brick.mat); i++) CHECK(ice.brick.mat[i] == bare.brick.mat[i]);
	for (int k = 0; k < ve::kBrickPaletteSize; k++) CHECK(ice.brick.palette[k] == bare.brick.palette[k]);

	// The union is untouched by all of this: the ice is solid where the raycast puts it.
	const ve::Sample u = ve::eval_field(gen, &add, 1, q.p[0], q.p[1], q.p[2]);
	CHECK(u.material == ve::material_id("ice"));
	CHECK(u.sdf < 0.0f);

	// The `opaque` flag off stores the UNION -- the build the marcher sees with the feature
	// off, where the ball is an ordinary solid surface. Guard: a bake site that selected the
	// opaque pair unconditionally would store air here and silently delete the feature.
	ve::BrickEval uni{};
	ve::eval_brick(gen, &add, 1, q.brick, &uni, nullptr, nullptr, false);
	CHECK(uni.brick.sdf[q.index] == ve::encode_sdf(-4.0f)); // the ball, solid (clamped)
	CHECK(uni.brick.sdf[q.index] != bare.brick.sdf[q.index]);
	CHECK(ve::decode_sdf(bare.brick.sdf[q.index]) == doctest::Approx(q.depth)); // the ground
	CHECK(ve::cell_state_field(gen, &add, 1, q.brick) == ve::kCellFull);
}

TEST_CASE("a transparent paint op contributes nothing to the opaque view") {
	ve::AnalyticGenerator gen;
	const SolidPoint q = deepest_solid(gen);
	const ve::EditOp paint = sphere(ve::kOpSpherePaint, ve::material_id("ice"), q.p, 4.0f);

	ve::BrickEval ice{}, bare{};
	ve::eval_brick(gen, &paint, 1, q.brick, &ice, nullptr, nullptr, true);
	ve::eval_brick(gen, nullptr, 0, q.brick, &bare, nullptr, nullptr, true);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(ice.brick.sdf[i] == bare.brick.sdf[i]);
	for (int k = 0; k < ve::kBrickPaletteSize; k++) CHECK(ice.brick.palette[k] == bare.brick.palette[k]);

	const ve::Sample u = ve::eval_field(gen, &paint, 1, q.p[0], q.p[1], q.p[2]);
	CHECK(u.material == ve::material_id("ice"));
}

TEST_CASE("a floating transparent ball leaves an all-air brick's opaque view untouched") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 b{25, 100, 37};
	const float c[3] = {b.x * ve::kBrickSize + 0.4f, b.y * ve::kBrickSize + 0.4f,
			b.z * ve::kBrickSize + 0.4f};
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f);
	ve::BrickEval ice{}, bare{};
	ve::eval_brick(gen, &add, 1, b, &ice, nullptr, nullptr, true);
	ve::eval_brick(gen, nullptr, 0, b, &bare, nullptr, nullptr, true);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(ice.brick.sdf[i] == bare.brick.sdf[i]);
	// Nothing for the marcher to find: every stored sample is air.
	const uint8_t zero = ve::encode_sdf(0.0f);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(ice.brick.sdf[i] > zero);
	// The union still holds it solid, which is what the collider and connectivity read.
	ve::BrickEval uni{};
	ve::eval_brick(gen, &add, 1, b, &uni, nullptr, nullptr, false);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(uni.brick.sdf[i] <= zero);
}

// The guard against a blanket "skip the op" reading of the opaque accumulator, read on the
// OPAQUE pair itself -- the union is asserted byte-identical to apply_ops elsewhere, so
// asserting only the union here could never fail.
TEST_CASE("an opaque add and a subtract still reach the opaque view") {
	ve::AnalyticGenerator gen;
	const SolidPoint q = deepest_solid(gen);
	ve::Sample s{}, o{};

	// 1 m inside a 1 m sphere, so the op beats the ground by a wide margin.
	const ve::EditOp rock = sphere(ve::kOpSphereAdd, ve::material_id("rock"), q.p, 1.0f);
	ve::eval_field_pair(gen, &rock, 1, q.p[0], q.p[1], q.p[2], &s, &o);
	CHECK(s.sdf == doctest::Approx(-1.0f));
	CHECK(s.material == ve::material_id("rock"));
	CHECK(o.sdf == doctest::Approx(-1.0f));
	CHECK(o.material == ve::material_id("rock"));

	const ve::EditOp sub = sphere(ve::kOpSphereSubtract, 0, q.p, 2.0f);
	ve::eval_field_pair(gen, &sub, 1, q.p[0], q.p[1], q.p[2], &s, &o);
	CHECK(s.sdf == doctest::Approx(2.0f));
	CHECK(s.material == 0);
	CHECK(o.sdf == doctest::Approx(2.0f));
	CHECK(o.material == 0);
}

// The same rule for PAINT, and the half nothing pinned: the skip is keyed on the PAINTED
// material, not on the op type, so painting an opaque material onto opaque solid must
// relabel it in the opaque bake too. (test_brick_diff.gd:113 pins the opaque ADD half.)
TEST_CASE("an opaque paint op still reaches the opaque bake") {
	ve::AnalyticGenerator gen;
	const SolidPoint q = deepest_solid(gen);
	// `bark` is tree-trunk material and never appears in the bare ground brick, so a cell
	// count of 0 against > 0 is unambiguous.
	const uint16_t bark = ve::material_id("bark");
	const ve::EditOp paint = sphere(ve::kOpSpherePaint, bark, q.p, 1.0f);

	ve::Sample s{}, o{};
	ve::eval_field_pair(gen, &paint, 1, q.p[0], q.p[1], q.p[2], &s, &o);
	CHECK(s.material == bark);
	CHECK(o.material == bark);      // the opaque accumulator applied it
	CHECK(o.sdf == doctest::Approx(s.sdf));  // and a paint moves no surface

	ve::BrickEval out{}, bare{};
	ve::eval_brick(gen, &paint, 1, q.brick, &out, nullptr, nullptr, true);
	ve::eval_brick(gen, nullptr, 0, q.brick, &bare, nullptr, nullptr, true);
	CHECK(count_cells(bare.brick, bark) == 0);
	CHECK(count_cells(out.brick, bark) > 0);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(out.brick.sdf[i] == bare.brick.sdf[i]);
}

// DELIBERATE, not an accident. A paint relabels EXISTING solid, so inside a transparent ADD
// op -- where the opaque view is air, because a transparent material is air -- there is
// nothing left to relabel and the opaque bake stays air. An ADD brings its own geometry and
// does reach the opaque view (the case above); that asymmetry is the whole point of pairing
// the accumulators. If this assertion ever has to flip, the rule changed with it.
TEST_CASE("an opaque paint inside a transparent add op relabels the union only") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 b{25, 100, 37};  // an all-air brick, so the ADD is the only solid
	float bo[3];
	ve::brick_world_origin(b, bo);
	const float c[3] = {bo[0] + 0.4f, bo[1] + 0.4f, bo[2] + 0.4f};
	const ve::EditOp ops[2] = {sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f),
			sphere(ve::kOpSpherePaint, ve::material_id("rock"), c, 0.5f)};

	ve::Sample s{}, o{};
	ve::eval_field_pair(gen, ops, 2, c[0], c[1], c[2], &s, &o);
	CHECK(s.material == ve::material_id("rock"));
	CHECK(s.sdf < 0.0f);
	CHECK(o.material != ve::material_id("rock"));
	CHECK(o.sdf > 0.0f);

	ve::BrickEval opaque{}, uni{};
	ve::eval_brick(gen, ops, 2, b, &opaque, nullptr, nullptr, true);
	ve::eval_brick(gen, ops, 2, b, &uni, nullptr, nullptr, false);
	const uint8_t zero = ve::encode_sdf(0.0f);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(opaque.brick.sdf[i] > zero);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(uni.brick.sdf[i] <= zero);
}

TEST_CASE("the union field is byte-identical to the untouched apply_ops path") {
	ve::AnalyticGenerator gen;
	const SolidPoint q = deepest_solid(gen);
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("ice"), q.p, 4.0f);
	const ve::Sample ref = ve::apply_ops(gen.sample(q.p[0], q.p[1], q.p[2]), &add, 1,
			q.p[0], q.p[1], q.p[2]);
	const ve::Sample got = ve::eval_field(gen, &add, 1, q.p[0], q.p[1], q.p[2]);
	CHECK(got.sdf == ref.sdf);
	CHECK(got.material == ref.material);
}

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
