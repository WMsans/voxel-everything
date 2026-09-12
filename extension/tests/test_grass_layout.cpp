#include <doctest/doctest.h>
#include "grass/grass_layout.h"
#include "grass/grass_settings.h"
#include "grass/grass_settings_store.h"
#include "world/brick.h"
#include <cmath>
#include <cstring>

namespace {
// An identity view_proj: the frustum planes it yields are the six faces of the NDC cube in
// world space. Enough to pin the extraction's signs without dragging a projection in.
void identity(float m[16]) {
	std::memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}
} // namespace

TEST_CASE("the brick box covers reach horizontally and vertical_reach vertically") {
	ve::GrassSettings s;
	s.reach_m = 40.0f;
	s.vertical_reach_m = 10.0f;
	const float cam[3] = {0.0f, 100.0f, 0.0f};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	// 40 m / 0.8 m = 50 bricks each way, plus the brick the camera sits in.
	CHECK(l.brick_min.x <= -50);
	CHECK(l.brick_max.x >= 50);
	CHECK(l.brick_min.z <= -50);
	CHECK(l.brick_max.z >= 50);
	// 100 m / 0.8 m = brick 125; +-10 m is +-12.5 bricks.
	CHECK(l.brick_min.y <= 112);
	CHECK(l.brick_max.y >= 137);
	// The box is much shorter than it is wide -- that is the whole point of the split.
	const int wide = l.brick_max.x - l.brick_min.x;
	const int tall = l.brick_max.y - l.brick_min.y;
	CHECK(tall < wide);
}

// Halving, not quartering. The old 4x-per-ring thinning took the default 16 down to
// 16/4/1/1, so ring 2 onward was one blade per 0.8 m brick -- a hole, not a tail. The far
// rings pay for the extra blades with width instead, via GrassParams::shape[3].
TEST_CASE("each ring past the first drops half its blades") {
	ve::GrassSettings s;
	s.blades_per_brick = 16;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.ring_count == 4);
	CHECK(l.blades_per_brick[0] == 16);
	CHECK(l.blades_per_brick[1] == 8);
	CHECK(l.blades_per_brick[2] == 4);
	CHECK(l.blades_per_brick[3] == 2);
}

// Thinning never reaches zero: a ring that draws nothing is a hole, not a saving. One
// blade per brick is the floor even when the shift would have run the count off the end.
TEST_CASE("ring thinning floors at one blade rather than zero") {
	ve::GrassSettings s;
	s.blades_per_brick = 1;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	for (int i = 0; i < l.ring_count; i++) CHECK(l.blades_per_brick[i] >= 1);
}

TEST_CASE("ring boundaries split the reach evenly and cover it") {
	ve::GrassSettings s;
	s.reach_m = 40.0f;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.ring_end_m[0] == doctest::Approx(10.0f));
	CHECK(l.ring_end_m[1] == doctest::Approx(20.0f));
	CHECK(l.ring_end_m[2] == doctest::Approx(30.0f));
	CHECK(l.ring_end_m[3] == doctest::Approx(40.0f));
	CHECK(ve::grass_ring_of(l, 0.0f) == 0);
	CHECK(ve::grass_ring_of(l, 9.9f) == 0);
	CHECK(ve::grass_ring_of(l, 10.1f) == 1);
	CHECK(ve::grass_ring_of(l, 39.9f) == 3);
	// Past the reach there is no ring; the caller must not place a blade there.
	CHECK(ve::grass_ring_of(l, 40.1f) < 0);
}

TEST_CASE("the blade estimate grows with reach and is capped by max_blades") {
	ve::GrassSettings s;
	s.reach_m = 20.0f;
	s.max_blades = 400000;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout near_l = ve::grass_layout(s, cam, vp);
	s.reach_m = 40.0f;
	const ve::GrassLayout far_l = ve::grass_layout(s, cam, vp);
	CHECK(far_l.estimated_blades > near_l.estimated_blades);

	s.max_blades = 1000;
	const ve::GrassLayout capped = ve::grass_layout(s, cam, vp);
	CHECK(capped.estimated_blades <= 1000);
}

TEST_CASE("disabled or zero-reach grass yields an empty box and no blades") {
	ve::GrassSettings s;
	s.enabled = false;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.max_bricks == 0);
	CHECK(l.estimated_blades == 0);
}

TEST_CASE("frustum planes point inward and are normalised") {
	ve::GrassSettings s;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	for (int i = 0; i < 6; i++) {
		const float *p = l.params.planes[i];
		const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		CHECK(len == doctest::Approx(1.0f));
		// The NDC-cube origin is inside every plane, so every signed distance is positive.
		CHECK(p[3] > 0.0f);
	}
}

TEST_CASE("GrassParams is 256 bytes and its floats land where GLSL expects") {
	// Sixteen vec4: cam, planes[6], brick_min, brick_dim, ring_end, ring_blades, blade,
	// wind, style, shape, limits. If this number moves, GRASS_PARAMS_BLOCK moved with it.
	CHECK(sizeof(ve::GrassParams) == 256);
	ve::GrassSettings s;
	const float cam[3] = {1.0f, 2.0f, 3.0f};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	const float *raw = reinterpret_cast<const float *>(&l.params);
	CHECK(raw[0] == doctest::Approx(1.0f));
	CHECK(raw[1] == doctest::Approx(2.0f));
	CHECK(raw[2] == doctest::Approx(3.0f));
}

// The shape block is what turns a field of randomly-pointed spikes into a field that lies
// one way. A zero lean spread would be a lawn of clones; a spread past pi is the old
// uniform-random azimuth wearing a different name.
// The camera tilt rides in style[2], the free slot of a block that is byte-pinned at 256
// bytes: the top-down fix costs no new vec4, no new field in the GLSL mirror and no layout
// change. Addressed through the store so this test does not also pin a C++ member name.
TEST_CASE("the style block carries the camera tilt") {
	ve::GrassSettingsStore store;
	REQUIRE(store.set_value("camera_tilt", 0.4f));
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(store.get(), cam, vp);
	CHECK(l.params.style[2] == doctest::Approx(0.4f));
}

// Blade lighting takes style[3], the block's last free float, for the same reason the tilt
// took style[2]: no new vec4, no layout change.
TEST_CASE("the style block carries the blade lighting blend") {
	ve::GrassSettingsStore store;
	REQUIRE(store.set_value("blade_lighting", 0.25f));
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(store.get(), cam, vp);
	CHECK(l.params.style[3] == doctest::Approx(0.25f));
}

TEST_CASE("the shape block carries the wind-alignment and blade-curve knobs") {
	ve::GrassSettings s;
	s.wind_dir_deg = 90.0f;
	s.lean_spread_rad = 0.5f;
	s.base_curve = 0.4f;
	s.ring_width_gain = 3.0f;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.params.shape[0] == doctest::Approx(1.57079633f)); // 90 degrees in radians
	CHECK(l.params.shape[1] == doctest::Approx(0.5f));
	CHECK(l.params.shape[2] == doctest::Approx(0.4f));
	CHECK(l.params.shape[3] == doctest::Approx(3.0f));
}
