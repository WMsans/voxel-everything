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

TEST_CASE("vertical_reach shortens the box back to a slab") {
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
	// Uncapped for the comparison: the far rings are on an ABSOLUTE schedule now, so their
	// share of the estimate does not shrink with the reach and 400k caps both layouts.
	s.max_blades = 4000000;
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

TEST_CASE("GrassParams is 272 bytes and its floats land where GLSL expects") {
	// Seventeen vec4: cam, planes[6], brick_min, brick_dim, ring_end, ring_blades, blade,
	// wind, style, shape, limits, far. If this number moves, the grass parameter block moved
	// with it.
	CHECK(sizeof(ve::GrassParams) == 272);
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

// Far LoD rings are what put grass past the brick atlas's residency radius. Cell size and
// radius double together, so every ring dispatches the SAME cell count -- that is the whole
// reason the dispatch stays affordable at eight times the near reach.
TEST_CASE("far LoD rings extend the reach without growing the per-ring cell count") {
	ve::GrassSettings s;
	s.reach_m = 40.0f;
	s.far_lod_rings = 3;
	s.far_blades_per_cell = 8;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.far_ring_count == 3);
	CHECK(l.far_reach_m == doctest::Approx(320.0f)); // 40 m doubled three times
	CHECK(l.far_cells == l.far_cell_dim * l.far_cell_dim);
	// One cell grid per ring on top of the near brick box, and nothing more.
	CHECK(l.max_bricks == l.near_brick_cap + 3 * l.far_cells);
	CHECK(l.params.far[0] == 3);
	CHECK(l.params.far[1] == l.far_cell_dim);
	CHECK(l.params.far[2] == l.far_cells);
	CHECK(l.params.far[3] == 8);
	// Stage 1 splits near from far on brick_dim.w, so it must be the NEAR thread count alone.
	CHECK(l.params.brick_dim[3] == l.near_columns);
}

// Doubling the reach doubles every far radius with it, so the cell count per ring does not
// move: the cost of far grass is linear in the RING COUNT, never in the radius.
TEST_CASE("far cell count per ring is independent of the reach") {
	ve::GrassSettings a;
	a.reach_m = 40.0f;
	ve::GrassSettings b = a;
	b.reach_m = 80.0f;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	CHECK(ve::grass_layout(b, cam, vp).far_cells >
			ve::grass_layout(a, cam, vp).far_cells / 2);
	// ...and adding a ring adds exactly one grid.
	ve::GrassSettings c = a;
	c.far_lod_rings = a.far_lod_rings + 1;
	const ve::GrassLayout la = ve::grass_layout(a, cam, vp);
	const ve::GrassLayout lc = ve::grass_layout(c, cam, vp);
	CHECK(lc.max_bricks == la.max_bricks + la.far_cells);
}

// The near box is as tall as it is wide by DEFAULT: grass covers everything the raymarcher
// draws, which is a sphere of resident bricks, not a slab around the camera's own height.
TEST_CASE("the default box is as tall as the reach") {
	ve::GrassSettings s;
	s.reach_m = 40.0f;
	const float cam[3] = {0.0f, 100.0f, 0.0f};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.brick_max.y - l.brick_min.y == l.brick_max.x - l.brick_min.x);
}

// Stage 1 walks a COLUMN per thread. A thread per brick over a box this tall would dispatch
// its volume: at a 40 m reach that is 101 x 101 x 101 threads against 101 x 101 columns.
TEST_CASE("stage 1 dispatches one thread per column, then one per far cell") {
	ve::GrassSettings s;
	s.reach_m = 40.0f;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	const int dim_x = l.brick_max.x - l.brick_min.x + 1;
	const int dim_y = l.brick_max.y - l.brick_min.y + 1;
	const int dim_z = l.brick_max.z - l.brick_min.z + 1;
	CHECK(l.near_columns == dim_x * dim_z);
	CHECK(l.dispatch_threads == l.near_columns + l.far_ring_count * l.far_cells);
	CHECK(l.dispatch_threads < dim_x * dim_y * dim_z);
	// Capacity is a STRICT bound, not a guess: the walk stops at kGrassColumnBricks per
	// column, so stage 1 can never want more brick list than was allocated.
	CHECK(l.near_brick_cap == ve::kGrassColumnBricks * l.near_columns);
	CHECK(l.max_bricks == l.near_brick_cap + l.far_ring_count * l.far_cells);
}

// The far schedule is absolute, so a reach that grows to the raymarcher's seam eats into the
// far rings instead of dragging them outward with it. That is what pays for the wider near
// field: the blade estimate barely moves even though the dense area doubles.
TEST_CASE("extending the reach to the seam does not multiply the blade budget") {
	ve::GrassSettings s;
	s.max_blades = 4000000; // uncapped, so the estimate itself is what is compared
	s.reach_m = 40.0f;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const int tuned = ve::grass_layout(s, cam, vp).estimated_blades;
	s.reach_m = 56.0f; // a typical fade-band seam (ve::lod_fade_band at a 60 m residency)
	const int seam = ve::grass_layout(s, cam, vp).estimated_blades;
	CHECK(seam > tuned);                                  // it does cover more ground
	CHECK(seam < tuned + tuned / 4);                      // ...without a second budget
	// A reach past a ring's whole annulus swallows it: ring 1 is 40..80 m, so at 80 m it
	// contributes nothing and the far rings start at ring 2.
	s.reach_m = 80.0f;
	const ve::GrassLayout wide = ve::grass_layout(s, cam, vp);
	CHECK(wide.far_reach_m == doctest::Approx(320.0f)); // ...and the far reach does not move
}

// The far ring schedule is absolute: 80 / 160 / 320 m, whatever the near reach is doing. A
// schedule keyed to the reach would coarsen every ring as the streamer caught up.
TEST_CASE("the far ring schedule does not follow the reach") {
	ve::GrassSettings a;
	a.reach_m = 40.0f;
	ve::GrassSettings b = a;
	b.reach_m = 56.0f;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout la = ve::grass_layout(a, cam, vp);
	const ve::GrassLayout lb = ve::grass_layout(b, cam, vp);
	CHECK(la.far_reach_m == doctest::Approx(lb.far_reach_m));
	CHECK(la.far_cell_dim == lb.far_cell_dim);
	CHECK(la.far_cells == lb.far_cells);
}

TEST_CASE("zero far rings leaves exactly the near-field brick box") {
	ve::GrassSettings s;
	s.far_lod_rings = 0;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.far_ring_count == 0);
	CHECK(l.max_bricks == l.near_brick_cap);
	CHECK(l.params.far[0] == 0);
}
