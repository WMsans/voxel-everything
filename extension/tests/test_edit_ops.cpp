#include <doctest/doctest.h>
#include "generator/edit_ops.h"

static ve::EditOp sphere(uint32_t type, float x, float y, float z, float r, uint32_t mat = 0) {
	ve::EditOp op{};
	op.type = type;
	op.material = mat;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

TEST_CASE("EditOp is exactly 32 bytes so the GPU pool can mirror it") {
	CHECK(sizeof(ve::EditOp) == 32);
}

TEST_CASE("sphere subtract carves solid into air and clears its material") {
	const ve::Sample solid{-1.0f, 2};
	const ve::EditOp op = sphere(ve::kOpSphereSubtract, 0, 0, 0, 5.0f);
	const ve::Sample in = ve::apply_op(solid, op, 0, 0, 0);   // 5 m inside the sphere
	CHECK(in.sdf == doctest::Approx(5.0f));
	CHECK(in.material == 0);
	const ve::Sample out = ve::apply_op(solid, op, 20, 0, 0); // far outside: untouched
	CHECK(out.sdf == doctest::Approx(-1.0f));
	CHECK(out.material == 2);
}

TEST_CASE("sphere add fills air and stamps its own material") {
	const ve::Sample air{4.0f, 0};
	const ve::EditOp op = sphere(ve::kOpSphereAdd, 0, 0, 0, 5.0f, 4);
	const ve::Sample in = ve::apply_op(air, op, 1, 0, 0);
	CHECK(in.sdf == doctest::Approx(-4.0f));
	CHECK(in.material == 4);
	// Where the existing solid is already closer to its own surface, it keeps its material.
	const ve::Sample deep{-9.0f, 2};
	CHECK(ve::apply_op(deep, op, 1, 0, 0).material == 2);
	CHECK(ve::apply_op(deep, op, 1, 0, 0).sdf == doctest::Approx(-9.0f));
}

TEST_CASE("sphere paint recolours solid only, never changes the surface") {
	const ve::Sample solid{-0.5f, 1};
	const ve::EditOp op = sphere(ve::kOpSpherePaint, 0, 0, 0, 5.0f, 3);
	const ve::Sample hit = ve::apply_op(solid, op, 1, 0, 0);
	CHECK(hit.material == 3);
	CHECK(hit.sdf == doctest::Approx(-0.5f));
	const ve::Sample air{0.5f, 0};
	CHECK(ve::apply_op(air, op, 1, 0, 0).material == 0); // air stays air-coloured
	CHECK(ve::apply_op(solid, op, 20, 0, 0).material == 1); // outside the brush
}

TEST_CASE("ops apply in order: a later add refills an earlier subtract") {
	const ve::EditOp ops[2] = {
		sphere(ve::kOpSphereSubtract, 0, 0, 0, 5.0f),
		sphere(ve::kOpSphereAdd, 0, 0, 0, 3.0f, 4),
	};
	const ve::Sample base{-1.0f, 2};
	const ve::Sample s = ve::apply_ops(base, ops, 2, 0, 0, 0);
	CHECK(s.sdf == doctest::Approx(-3.0f)); // the add won at the centre
	CHECK(s.material == 4);
	// Reversing the order leaves a hole: the subtract runs last.
	const ve::EditOp rev[2] = {ops[1], ops[0]};
	CHECK(ve::apply_ops(base, rev, 2, 0, 0, 0).sdf == doctest::Approx(5.0f));
}

TEST_CASE("apply_ops with zero ops is the identity") {
	const ve::Sample base{-1.0f, 2};
	const ve::Sample s = ve::apply_ops(base, nullptr, 0, 1, 2, 3);
	CHECK(s.sdf == doctest::Approx(-1.0f));
	CHECK(s.material == 2);
}

TEST_CASE("touched ranges cover the sphere plus one voxel of apron margin") {
	// A brick's SDF lattice reaches one voxel past its own extent (kBrickSdfStride == 17),
	// so an op grazing that plane still changes the brick's stored bytes.
	const ve::EditOp op = sphere(ve::kOpSphereSubtract, 8.0f, 8.0f, 8.0f, 1.0f);
	ve::IVec3 lo{}, hi{};
	ve::op_brick_range(op, &lo, &hi);
	CHECK(lo == ve::IVec3{8, 8, 8});   // floor((8 - 1.05) / 0.8) = 8
	CHECK(hi == ve::IVec3{11, 11, 11}); // floor((8 + 1.05) / 0.8) = 11
	ve::IVec3 rlo{}, rhi{};
	ve::op_region_range(op, &rlo, &rhi);
	CHECK(rlo == ve::IVec3{0, 0, 0});
	CHECK(rhi == ve::IVec3{0, 0, 0});
}

TEST_CASE("touched ranges floor correctly below the origin plane") {
	const ve::EditOp op = sphere(ve::kOpSphereSubtract, -0.5f, -30.0f, 0.5f, 1.0f);
	ve::IVec3 lo{}, hi{};
	ve::op_brick_range(op, &lo, &hi);
	CHECK(lo.x == -2);
	CHECK(hi.x == 0);
	ve::IVec3 rlo{}, rhi{};
	ve::op_region_range(op, &rlo, &rhi);
	CHECK(rlo.y == -2); // -31.05 m / 25.6 m -> region -2
	CHECK(rhi.y == -2); // -28.95 m / 25.6 m -> region -2
}
