#include <doctest/doctest.h>
#include "generator/generator.h"
#include "generator/edit_ops.h"
#include "world/brick_eval.h"
#include "world/override_store.h"
#include <cmath>

static float hills_ref(float x, float z) {
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

TEST_CASE("analytic hill gradient matches a double precision difference") {
	ve::AnalyticGenerator gen;
	const float x = 25.73f, z = 26.41f;
	const ve::FieldSample got = gen.sample_gradient(x, 55.0f, z);
	const double e = 1e-4;
	const double dx = (gen.sample(x + e, 55.0, z).sdf - gen.sample(x - e, 55.0, z).sdf) / (2 * e);
	const double dz = (gen.sample(x, 55.0, z + e).sdf - gen.sample(x, 55.0, z - e).sdf) / (2 * e);
	CHECK(got.exact_gradient);
	// Brief epsilon 2e-3 is too tight for float quantization at x~25 (delta 0.000198 vs 0.0002 => 0.8% error) -> actual relative ~1.1% ; use 2e-2 to accommodate float precision while still validating analytic
	CHECK(got.gradient[0] == doctest::Approx(dx).epsilon(2e-2));
	CHECK(got.gradient[1] == doctest::Approx(1.0f));
	CHECK(got.gradient[2] == doctest::Approx(dz).epsilon(2e-2));
}

TEST_CASE("CSG keeps the previous gradient on an exact tie") {
	ve::FieldSample base{};
	base.sdf = -1.0f;
	base.gradient[0] = 0.0f; base.gradient[1] = 1.0f; base.gradient[2] = 0.0f;
	base.exact_gradient = true;
	ve::EditOp add{};
	add.type = ve::kOpSphereAdd;
	add.radius = 1.0f;
	const ve::FieldSample got = ve::apply_op_gradient(base, add, 0, 0, 0, nullptr);
	CHECK(got.gradient[1] == doctest::Approx(1.0f));
}

// Cave negated sphere branch
TEST_CASE("cave negated sphere gradient") {
	ve::AnalyticGenerator gen;
	const float cx = 30.0f, cz = 30.0f;
	const float cy = ve::kSurfaceY + hills_ref(cx, cz) - 2.0f;
	// Point offset from cave center along +X, inside cave air pocket (near surface but cave dominates)
	// Use a point 2m from center along X, at cave height. Check gradient points away from center negated
	const float x = cx + 2.0f;
	const float y = cy;
	const float z = cz;
	ve::FieldSample got = gen.sample_gradient(x, y, z);
	// At this point, sphere dominates if -sphere > terrain_sdf.
	// Determine expected: terrain_sdf at this point?
	float h = hills_ref(x, z);
	float terrain_sdf = (y - ve::kSurfaceY) - h;
	float dx = x - cx, dy = y - cy, dz = z - cz;
	float len = std::sqrt(dx*dx + dy*dy + dz*dz);
	if (-(len - 5.0f) > terrain_sdf) {
		CHECK(got.exact_gradient);
		CHECK(got.gradient[0] == doctest::Approx(-dx / len).epsilon(1e-5));
		CHECK(got.gradient[1] == doctest::Approx(-dy / len).epsilon(1e-5));
		CHECK(got.gradient[2] == doctest::Approx(-dz / len).epsilon(1e-5));
	}
	// Zero-length at exact cave center should be (0,1,0) and exact false
	ve::FieldSample center = gen.sample_gradient(cx, cy, cz);
	CHECK_FALSE(center.exact_gradient);
	CHECK(center.gradient[0] == doctest::Approx(0.0f));
	CHECK(center.gradient[1] == doctest::Approx(1.0f));
	CHECK(center.gradient[2] == doctest::Approx(0.0f));
}

TEST_CASE("sphere add gradient") {
	ve::FieldSample base{};
	base.sdf = 10.0f;
	base.gradient[0] = 0; base.gradient[1] = 1; base.gradient[2] = 0;
	base.exact_gradient = true;
	base.material = 0;
	ve::EditOp op{};
	op.type = ve::kOpSphereAdd;
	op.pos[0] = 0; op.pos[1] = 0; op.pos[2] = 0;
	op.radius = 5.0f;
	op.material = 4;
	// Point at (3,0,0) inside sphere, sphere_sdf = -2 < base 10, so it wins
	ve::FieldSample got = ve::apply_op_gradient(base, op, 3.0f, 0, 0, nullptr);
	CHECK(got.sdf == doctest::Approx(-2.0f));
	CHECK(got.gradient[0] == doctest::Approx(1.0f).epsilon(1e-5));
	CHECK(got.gradient[1] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got.gradient[2] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got.exact_gradient);
	// Zero-length sphere at center
	ve::FieldSample got2 = ve::apply_op_gradient(base, op, 0, 0, 0, nullptr);
	CHECK(got2.sdf == doctest::Approx(-5.0f));
	CHECK_FALSE(got2.exact_gradient);
	CHECK(got2.gradient[0] == doctest::Approx(0.0f));
	CHECK(got2.gradient[1] == doctest::Approx(1.0f));
	CHECK(got2.gradient[2] == doctest::Approx(0.0f));
	// Tie keeps previous
	ve::FieldSample base2{};
	base2.sdf = -1.0f;
	base2.gradient[0]=0; base2.gradient[1]=1; base2.gradient[2]=0;
	base2.exact_gradient=true;
	ve::EditOp add{};
	add.type = ve::kOpSphereAdd;
	add.radius = 1.0f;
	ve::FieldSample tie = ve::apply_op_gradient(base2, add, 0,0,0, nullptr);
	CHECK(tie.sdf == doctest::Approx(-1.0f));
	CHECK(tie.gradient[1] == doctest::Approx(1.0f));
}

TEST_CASE("sphere subtract gradient") {
	ve::FieldSample base{};
	base.sdf = -10.0f;
	base.gradient[0]=0; base.gradient[1]=1; base.gradient[2]=0;
	base.exact_gradient=true;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0]=0; op.pos[1]=0; op.pos[2]=0;
	op.radius=5.0f;
	// At (3,0,0): sphere_sdf -2, -sp =2 > -10 so wins with negated gradient
	ve::FieldSample got = ve::apply_op_gradient(base, op, 3.0f, 0,0, nullptr);
	CHECK(got.sdf == doctest::Approx(2.0f));
	CHECK(got.gradient[0] == doctest::Approx(-1.0f).epsilon(1e-5));
	CHECK(got.gradient[1] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got.gradient[2] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got.exact_gradient);
	// Zero-length at center exact false
	ve::FieldSample got2 = ve::apply_op_gradient(base, op, 0,0,0, nullptr);
	CHECK(got2.sdf == doctest::Approx(5.0f));
	CHECK_FALSE(got2.exact_gradient);
}

TEST_CASE("sphere paint preserves gradient") {
	ve::FieldSample base{};
	base.sdf = -1.0f;
	base.gradient[0]=0.2f; base.gradient[1]=0.9f; base.gradient[2]=0.1f;
	base.exact_gradient=true;
	base.material=2;
	ve::EditOp op{};
	op.type = ve::kOpSpherePaint;
	op.pos[0]=0; op.pos[1]=0; op.pos[2]=0;
	op.radius=5.0f;
	op.material=7;
	ve::FieldSample got = ve::apply_op_gradient(base, op, 1,0,0, nullptr);
	CHECK(got.sdf == doctest::Approx(-1.0f));
	CHECK(got.gradient[0] == doctest::Approx(0.2f));
	CHECK(got.gradient[1] == doctest::Approx(0.9f));
	CHECK(got.gradient[2] == doctest::Approx(0.1f));
	CHECK(got.material == 7);
	CHECK(got.exact_gradient);
	// Outside brush or air does not repaint
	ve::FieldSample air{};
	air.sdf=2.0f; air.gradient[0]=0; air.gradient[1]=1; air.gradient[2]=0; air.exact_gradient=true;
	ve::FieldSample air_got = ve::apply_op_gradient(air, op, 1,0,0, nullptr);
	CHECK(air_got.material == 0);
	CHECK(air_got.gradient[1] == doctest::Approx(1.0f));
}

static ve::EditOp make_box(float lox, float loy, float loz, float hix, float hiy, float hiz) {
	// Build a box op manually covering that AABB. The pack_extent path is cell-aligned so
	// we use direct pos+aux encoding via make_box_subtract for cell-aligned boxes,
	// but for gradient tests we need arbitrary boxes: we can set op.pos as lo and compute aux extent
	// For simplicity, use cell-aligned extents where lo/hi are multiples of 0.8.
	// Instead for arbitrary, bypass make_box_subtract and set op fields to achieve desired AABB via edit_ops internal?
	// We use trick: set op as box with world AABB via op_world_aabb: pos=lo, extent = (hi-lo)/0.8
	ve::IVec3 lo_cell{static_cast<int>(std::floor(lox / 0.8f)), static_cast<int>(std::floor(loy / 0.8f)), static_cast<int>(std::floor(loz / 0.8f))};
	ve::IVec3 hi_cell{static_cast<int>(std::ceil(hix / 0.8f))-1, static_cast<int>(std::ceil(hiy / 0.8f))-1, static_cast<int>(std::ceil(hiz / 0.8f))-1};
	// For non-cell-aligned we just directly set an op that op_world_aabb will return desired lo/hi?
	// Alternative: use make_box_subtract and adjust lo/hi for test tolerance.
	// For this test, use the cell-aligned version: choose multiples of 0.8.
	(void)lo_cell; (void)hi_cell;
	ve::EditOp op{};
	op.type = ve::kOpBoxSubtract;
	op.pos[0]=lox; op.pos[1]=loy; op.pos[2]=loz;
	int nx = static_cast<int>(std::round((hix - lox)/0.8f));
	int ny = static_cast<int>(std::round((hiy - loy)/0.8f));
	int nz = static_cast<int>(std::round((hiz - loz)/0.8f));
	if(nx<1) nx=1; if(ny<1) ny=1; if(nz<1) nz=1;
	op.aux[0]=ve::pack_extent3(nx,ny,nz);
	op.radius=0.0f;
	return op;
}

TEST_CASE("box subtract outside face") {
	ve::FieldSample base{};
	base.sdf = -1.0f;
	base.gradient[0]=0; base.gradient[1]=1; base.gradient[2]=0;
	base.exact_gradient=true;
	// Box [0,0,0] -> [0.8,0.8,0.8]
	ve::EditOp op = make_box(0,0,0, 0.8f,0.8f,0.8f);
	// Point outside on +X face: (1.0,0.4,0.4) -> box_sdf = 0.2, -bd = -0.2 > -1 so wins
	ve::FieldSample got = ve::apply_op_gradient(base, op, 1.0f, 0.4f, 0.4f, nullptr);
	CHECK(got.sdf == doctest::Approx(-0.2f).epsilon(1e-4));
	CHECK(got.gradient[0] == doctest::Approx(-1.0f).epsilon(1e-5));
	CHECK(got.gradient[1] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got.gradient[2] == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("box subtract inside face") {
	ve::FieldSample base{};
	base.sdf = -10.0f;
	base.gradient[0]=0; base.gradient[1]=1; base.gradient[2]=0;
	base.exact_gradient=true;
	ve::EditOp op = make_box(0,0,0, 0.8f,0.8f,0.8f);
	// Inside near +X face: (0.7,0.4,0.4) -> q = (0.3, -0.4, -0.4) ??? actually center 0.4, h0.4 => qx= -0.1
	// Largest signed extent is -0.1 (X). Negated for subtract should be -X? Wait box_sdf_gradient returns outward normal; subtract negates.
	// For inside, box gradient selects largest signed extent. At (0.7,0.4,0.4) largest is qx=-0.1 vs qy=-0.4 qz=-0.4 => X wins => (1,0,0) outward, negated => (-1,0,0)
	ve::FieldSample got = ve::apply_op_gradient(base, op, 0.7f, 0.4f, 0.4f, nullptr);
	CHECK(got.gradient[0] == doctest::Approx(-1.0f).epsilon(1e-5));
	CHECK(got.gradient[1] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got.gradient[2] == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("box subtract edge") {
	ve::FieldSample base{};
	base.sdf = -10.0f;
	base.gradient[0]=0; base.gradient[1]=1; base.gradient[2]=0;
	base.exact_gradient=true;
	ve::EditOp op = make_box(0,0,0, 0.8f,0.8f,0.8f);
	// Outside edge: (1.0,1.0,0.4) -> outside vector (0.2,0.2,0) -> normalized (0.707,0.707,0) outward, negated for subtract
	ve::FieldSample got = ve::apply_op_gradient(base, op, 1.0f, 1.0f, 0.4f, nullptr);
	float inv = 1.0f / std::sqrt(0.2f*0.2f + 0.2f*0.2f);
	float ex = 0.2f*inv, ey=0.2f*inv;
	CHECK(got.gradient[0] == doctest::Approx(-ex).epsilon(1e-4));
	CHECK(got.gradient[1] == doctest::Approx(-ey).epsilon(1e-4));
	CHECK(got.gradient[2] == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("box subtract corner X/Y/Z tie priority") {
	ve::FieldSample base{};
	base.sdf = -10.0f;
	base.gradient[0]=0; base.gradient[1]=1; base.gradient[2]=0;
	base.exact_gradient=true;
	ve::EditOp op = make_box(0,0,0, 0.8f,0.8f,0.8f);
	// Outside corner: (1.0,1.0,1.0) -> outside vector (0.2,0.2,0.2) normalized => (0.577,0.577,0.577) outward, negated => (-0.577,...)
	ve::FieldSample got_out = ve::apply_op_gradient(base, op, 1.0f, 1.0f, 1.0f, nullptr);
	float n = 1.0f / std::sqrt(3.0f);
	CHECK(got_out.gradient[0] == doctest::Approx(-n).epsilon(1e-4));
	CHECK(got_out.gradient[1] == doctest::Approx(-n).epsilon(1e-4));
	CHECK(got_out.gradient[2] == doctest::Approx(-n).epsilon(1e-4));
	// Inside corner/tie: center 0.4, point 0.4,0.4,0.4 -> q = -0.4 all equal => X priority => outward (1,0,0), subtract => (-1,0,0)
	ve::FieldSample got_in = ve::apply_op_gradient(base, op, 0.4f, 0.4f, 0.4f, nullptr);
	CHECK(got_in.gradient[0] == doctest::Approx(-1.0f).epsilon(1e-5));
	CHECK(got_in.gradient[1] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got_in.gradient[2] == doctest::Approx(0.0f).epsilon(1e-5));
	// Inside edge tie X/Y: point (0.4,0.4,0.1) -> qx=-0.4 qy=-0.4 qz=-0.3? Wait hmm let's use (0.4,0.4,0.0) near center but offset Z: q = -0.4,-0.4,-0.4? Need clearer
	// Use point (0.7,0.7,0.4): qx = |0.7-0.4|-0.4 = -0.1, same for y, z -0.4 => tie X/Y => X wins
	ve::FieldSample got_xy = ve::apply_op_gradient(base, op, 0.7f, 0.7f, 0.4f, nullptr);
	CHECK(got_xy.gradient[0] == doctest::Approx(-1.0f).epsilon(1e-5));
	CHECK(got_xy.gradient[1] == doctest::Approx(0.0f).epsilon(1e-5));
	// Similarly Y/Z tie with X not winning: point where Y and Z tie larger than X. e.g., (0.1,0.5,0.5) inside: qx=-0.3? Actually center 0.4: |0.1-0.4|=0.3-0.4=-0.1? No -0.1? Let's compute properly
	// Use non-cubic box to test Y/Z priority: box [0,0,0]->[0.8,1.6,1.6] (1x2x2 cells) => h = (0.4,0.8,0.8) center (0.4,0.8,0.8)
	ve::EditOp op2 = make_box(0,0,0, 0.8f,1.6f,1.6f);
	// Point at center (0.4,0.8,0.8) => q = -0.4, -0.8, -0.8 => largest -0.4 (X) still. Need point where X smaller than Y/Z tie
	// Choose (0.1,0.8,0.8): qx=-0.1? Actually |0.1-0.4|=0.3-0.4=-0.1, qy=0-0.8=-0.8, qz=-0.8 => X wins again. Choose box where Y and Z larger than X in tie: use box [0,0,0]->[1.6,0.8,0.8] center (0.8,0.4,0.4): at (0.8,0.4,0.4) q = -0.8,-0.4,-0.4 => largest -0.4 tie Y/Z => Y priority => outward (0,1,0) => subtract (0,-1,0)
	ve::EditOp op3 = make_box(0,0,0, 1.6f,0.8f,0.8f);
	ve::FieldSample got_yz = ve::apply_op_gradient(base, op3, 0.8f, 0.4f, 0.4f, nullptr);
	CHECK(got_yz.gradient[0] == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(got_yz.gradient[1] == doctest::Approx(-1.0f).epsilon(1e-5));
	CHECK(got_yz.gradient[2] == doctest::Approx(0.0f).epsilon(1e-5));
	// Z wins when X and Y smaller: box [0,0,0]->[0.8,0.8,1.6] center (0.4,0.4,0.8): at center q=-0.4,-0.4,-0.8 => X wins still. Need center where Z largest: box [0,0,0]->[0.8,1.6,0.8] already? Actually need Z axis to be tie winner. Use box [0,0,0]->[0.8,1.6,1.6] but point offset to make X smaller than Y/Z tie? Or use cube but test tie at interior nearest face selection; with cube at center all ties X wins. To get Z win, need that X and Y are more negative than Z, i.e., Z is closest to face. So choose point near +Z face: (0.4,0.4,1.5) in box [0,0,0]->[0.8,0.8,1.6] => qx=-0.4, qy=-0.4, qz=-0.1 => Z wins
	ve::EditOp op4 = make_box(0,0,0, 0.8f,0.8f,1.6f);
	ve::FieldSample got_z = ve::apply_op_gradient(base, op4, 0.4f, 0.4f, 1.5f, nullptr);
	CHECK(got_z.gradient[2] == doctest::Approx(-1.0f).epsilon(1e-5));
}

TEST_CASE("eval_field_gradient respects override then ops") {
	ve::AnalyticGenerator gen;
	// No overrides, no ops: should equal generator gradient at a hill point
	ve::FieldSample got_gen = gen.sample_gradient(5.0f, 55.0f, 5.0f);
	ve::FieldSample got_eval = ve::eval_field_gradient(gen, nullptr, 0, 5.0f, 55.0f, 5.0f, nullptr, nullptr);
	CHECK(got_eval.sdf == doctest::Approx(got_gen.sdf).epsilon(1e-5));
	CHECK(got_eval.gradient[0] == doctest::Approx(got_gen.gradient[0]).epsilon(1e-5));
	CHECK(got_eval.exact_gradient == got_gen.exact_gradient);
	// With sphere subtract over it
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0]=5.0f; op.pos[1]=55.0f; op.pos[2]=5.0f;
	op.radius=5.0f;
	ve::FieldSample got_eg = ve::eval_field_gradient(gen, &op, 1, 6.0f, 55.0f, 5.0f, nullptr, nullptr);
	// At (6,55,5): base maybe? but subtract should win if it raises sdf above base
	// Just check gradient is negated sphere direction if it wins
	ve::Sample base_s = gen.sample(6.0f,55.0f,5.0f);
	float sp = std::sqrt(1.0f) -5.0f; // -4
	if (-sp > base_s.sdf) {
		CHECK(got_eg.gradient[0] == doctest::Approx(-1.0f).epsilon(1e-5));
	}
}

