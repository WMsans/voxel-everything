#include <doctest/doctest.h>
#include "shade/sun_cascades.h"
#include "shade/sun_ortho.h"
#include "shade/cel.h"
#include "lod/lod_grid.h"
#include <cmath>

// The derivation, stated once (spec section 2):
//   r0 = kLodBaseCell * (kSize - 1) / 2      cascade 0's texel IS the finest LoD cell
//   rN = stream_radius_m                      the outermost IS what ships today
//   r1 = sqrt(r0 * rN)                        constant ratio between texel sizes
TEST_CASE("the cascade radii and texels derive from the base cell and the stream radius") {
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(4000.0f, 2048, c);
	REQUIRE(n == 3);

	CHECK(c[0].radius == doctest::Approx(409.4f).epsilon(1e-5));
	CHECK(c[1].radius == doctest::Approx(1279.687f).epsilon(1e-5));
	CHECK(c[2].radius == doctest::Approx(4000.0f).epsilon(1e-6));

	CHECK(c[0].texel_world == doctest::Approx(0.400000f).epsilon(1e-5));
	CHECK(c[1].texel_world == doctest::Approx(1.250305f).epsilon(1e-5));
	CHECK(c[2].texel_world == doctest::Approx(3.908158f).epsilon(1e-5));
}

// Cascade 0's texel equals kLodBaseCell EXACTLY in real arithmetic, so evaluating
// "lod_cell_size(0) <= texel_world" for it is a float-rounding coin flip that would
// silently coarsen every near shadow. It is hard-wired instead.
TEST_CASE("cascade 0 is unclamped by construction, not by evaluating the rule") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(4000.0f, 2048, c) == 3);
	CHECK(c[0].min_level == 0);
}

// Descending below the texel is work whose result cannot be resolved: at cascade 2's
// 3.908 m texel a level-0 chunk is three texels across.
TEST_CASE("min_level stops the cut once a cell is finer than a shadow texel") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(4000.0f, 2048, c) == 3);
	CHECK(c[1].min_level == 1); // cell 0.8 <= 1.250 < cell 1.6
	CHECK(c[2].min_level == 3); // cell 3.2 <= 3.908 < cell 6.4
	for (int i = 0; i < 3; i++) {
		CHECK(ve::lod_cell_size(c[i].min_level) <= c[i].texel_world + 1e-4f);
		if (c[i].min_level + 1 < ve::kLodLevels)
			CHECK(ve::lod_cell_size(c[i].min_level + 1) > c[i].texel_world);
	}
}

// THE no-regression property. Whatever the radius, the outermost cascade is the map that
// ships today, produced by the same call with the same arguments.
TEST_CASE("the outermost cascade is exactly today's single map") {
	for (const float radius : {1638.4f, 2500.0f, 4000.0f}) {
		ve::SunCascade c[ve::kSunCascades];
		const int n = ve::sun_cascades(radius, 2048, c);
		REQUIRE(n >= 1);
		CHECK(c[n - 1].radius == doctest::Approx(radius).epsilon(1e-6));

		const float cam[3] = {800.0f, 60.0f, 800.0f};
		const ve::SunOrtho shipped = ve::sun_ortho_sphere(ve::kSunDir, cam, radius, 2048);
		REQUIRE(shipped.valid);
		CHECK(c[n - 1].texel_world == doctest::Approx(shipped.texel_world).epsilon(1e-6));
	}
}

// At the OLD default the whole table is a clean doubling, and cascade 2 reproduces the
// 1.601 m texel the characterization test pinned in Task 0.
TEST_CASE("at the old default radius the table is 409.4 / 819.0 / 1638.4") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(1638.4f, 2048, c) == 3);
	CHECK(c[0].radius == doctest::Approx(409.4f).epsilon(1e-5));
	CHECK(c[1].radius == doctest::Approx(819.0f).epsilon(1e-4));
	CHECK(c[2].radius == doctest::Approx(1638.4f).epsilon(1e-6));
	CHECK(c[2].texel_world == doctest::Approx(1.600782f).epsilon(1e-5));
}

// The degenerate case IS the old case: too small a radius to split means one map, and one
// map is exactly what shipped before cascades existed.
TEST_CASE("a radius inside cascade 0 collapses the set to a single cascade") {
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(200.0f, 2048, c);
	REQUIRE(n == 1);
	CHECK(c[0].radius == doctest::Approx(200.0f).epsilon(1e-6));
	CHECK(c[0].min_level == 0);
	CHECK(c[0].texel_world == doctest::Approx(400.0f / 2047.0f).epsilon(1e-6));
}

TEST_CASE("unusable inputs report no cascades rather than a degenerate matrix") {
	ve::SunCascade c[ve::kSunCascades];
	CHECK(ve::sun_cascades(0.0f, 2048, c) == 0);
	CHECK(ve::sun_cascades(-1.0f, 2048, c) == 0);
	CHECK(ve::sun_cascades(4000.0f, 1, c) == 0);
	CHECK(ve::sun_cascades(4000.0f, 0, c) == 0);
}

// The middle cascade is the geometric mean, so texel size steps by a CONSTANT RATIO
// rather than by a chosen constant. This is what makes the table derived, not tuned.
TEST_CASE("the radii are a geometric progression") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(4000.0f, 2048, c) == 3);
	CHECK(c[1].radius * c[1].radius ==
			doctest::Approx(c[0].radius * c[2].radius).epsilon(1e-4));
	CHECK(c[1].radius / c[0].radius ==
			doctest::Approx(c[2].radius / c[1].radius).epsilon(1e-4));
}
