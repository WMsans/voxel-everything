#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// Executes the ACTUAL shaders/conifer.glslh natively, as test_tree_shader.cpp executes
// tree.glslh. The synthetic ground is a plane, h = 10 + slope_x * x, so every bound below is
// checked against geometry whose answer is known.
namespace {
namespace cs {
#include "terrain/glsl_shim.h"
// terraced: 12 m flat ledges (trees stand there) between 4 m cliffs of slope 8.5 -- the
// fjord-wall case Review Focus 1 is about, kept inside the field's 8.6 slope bound.
struct Eval {
	float slope_x = 0.0f;
	bool terraced = false;
	vec4 conifer_ground(vec2 xz) {
		if (!terraced) return vec4(10.0f + slope_x * xz.x, slope_x, 0.0f, 0.0f);
		const float k = floor(xz.x / 16.0f);
		const float u = xz.x - k * 16.0f;
		const float cliff = u > 12.0f ? u - 12.0f : 0.0f;
		return vec4(10.0f + k * 34.0f + cliff * 8.5f, u > 12.0f ? 8.5f : 0.0f, 0.0f, 0.0f);
	}
#include "../../shaders/conifer.glslh"
};
} // namespace cs

constexpr float kSurfaceY = 51.2f;

cs::Eval::ConiferParams params(cs::Eval &ev) {
	// fjords.pipeline's values: water 51.2, shore 2, snow line 380 +- 25, ridge drop 40.
	return ev.conifer_params_make(8.0f, 0.85f, 18.0f, 28.0f, 0.45f, 3.0f, 1.7f, 40.0f,
			51.2f, 2.0f, 380.0f, 25.0f, 40.0f, kSurfaceY);
}

// Every present tree whose foot is within `radius` metres of p in XZ, brute force: the
// answer conifer_walk and conifer_far_bound must never overstate.
float brute_trunk_distance(cs::Eval &ev, const cs::Eval::ConiferParams &cp, cs::vec3 p, int radius_cells) {
	float best = 1.0e30f;
	const int bx = int(std::floor(p.x / cp.cell)), bz = int(std::floor(p.z / cp.cell));
	for (int dz = -radius_cells; dz <= radius_cells; dz++)
		for (int dx = -radius_cells; dx <= radius_cells; dx++) {
			const cs::ivec2 cell(bx + dx, bz + dz);
			if (!ev.conifer_cell_gate(cell, cp)) continue;
			const cs::vec2 xz = ev.conifer_cell_xz(cell, cp);
			const cs::vec4 g = ev.conifer_ground(xz);
			const float slope = std::sqrt(g.y * g.y + g.z * g.z);
			if (!ev.conifer_ground_ok(cp.surface_y + g.x - cp.water_y, slope, cp)) continue;
			const auto c = ev.conifer_at(cell, xz, cp.surface_y + g.x, slope, cp);
			best = std::fmin(best, ev.conifer_trunk_sdf(p, c));
		}
	return best;
}

uint32_t lcg(uint32_t &s) { s = s * 1664525u + 1013904223u; return s; }
float frand(uint32_t &s, float lo, float hi) { return lo + (hi - lo) * float(lcg(s) >> 8) / 16777216.0f; }
} // namespace

TEST_CASE("conifer placement is deterministic and never leaves its cell's jitter box") {
	cs::Eval ev;
	const auto cp = params(ev);
	for (int x : {-9, 0, 3, 77, -40000})
		for (int z : {-4, 0, 11, 250, 31000}) {
			const cs::vec2 a = ev.conifer_cell_xz(cs::ivec2(x, z), cp);
			const cs::vec2 b = ev.conifer_cell_xz(cs::ivec2(x, z), cp);
			CHECK(a.x == b.x);
			CHECK(a.y == b.y);
			const float cx = (float(x) + 0.5f) * cp.cell, cz = (float(z) + 0.5f) * cp.cell;
			CHECK(std::fabs(a.x - cx) <= 0.35f * cp.cell + 1e-3f);
			CHECK(std::fabs(a.y - cz) <= 0.35f * cp.cell + 1e-3f);
		}
}

TEST_CASE("the grove gate keeps most cells and opens clearings") {
	cs::Eval ev;
	const auto cp = params(ev);
	int kept = 0, total = 0;
	for (int z = 0; z < 120; z++)
		for (int x = 0; x < 120; x++) {
			kept += ev.conifer_cell_gate(cs::ivec2(x, z), cp);
			total++;
		}
	const float frac = float(kept) / float(total);
	CHECK(frac > 0.45f);
	// Below the 0.85 density by the clearings' share; a gate that ignored the grove noise
	// would land on 0.85.
	CHECK(frac < 0.83f);
}

TEST_CASE("the ground gate takes shore, tree line and slope from the fjord bands") {
	cs::Eval ev;
	const auto cp = params(ev);
	CHECK(cp.shore_e == doctest::Approx(3.0f));
	CHECK(cp.treeline_e == doctest::Approx(380.0f - 25.0f - 40.0f - 40.0f));
	CHECK(ev.conifer_ground_ok(50.0f, 0.5f, cp));
	CHECK_FALSE(ev.conifer_ground_ok(2.5f, 0.5f, cp));    // shore band
	CHECK_FALSE(ev.conifer_ground_ok(-64.0f, 0.5f, cp));  // the non-resident fallback height
	CHECK_FALSE(ev.conifer_ground_ok(300.0f, 0.5f, cp));  // above the tree line
	CHECK(ev.conifer_ground_ok(50.0f, 1.65f, cp));        // steep walls carry forest
	CHECK_FALSE(ev.conifer_ground_ok(50.0f, 1.75f, cp));
}

TEST_CASE("the tier profile never leaves 1.15 x the envelope and is zero off the crown") {
	cs::Eval ev;
	const auto cp = params(ev);
	for (int i = 0; i < 400; i++) {
		const auto c = ev.conifer_at(cs::ivec2(i, -i * 3), cs::vec2(0.0f, 0.0f), 60.0f, 0.3f, cp);
		CHECK(c.tiers >= 6.0f);
		CHECK(c.tiers <= 9.0f);
		CHECK(c.R <= cp.crown_radius * 1.25f + 1e-4f);
		CHECK(ev.conifer_profile(c, -0.01f) == 0.0f);
		CHECK(ev.conifer_profile(c, 1.01f) == 0.0f);
		for (int k = 0; k <= 200; k++) {
			const float s = float(k) / 200.0f;
			CHECK(ev.conifer_profile(c, s) <= 1.15f * c.R * (1.0f - s) + 1e-4f);
		}
	}
}

TEST_CASE("a trunk is 1-Lipschitz along secants") {
	cs::Eval ev;
	const auto cp = params(ev);
	const auto c = ev.conifer_at(cs::ivec2(3, 4), cs::vec2(28.0f, 36.0f), 70.0f, 0.8f, cp);
	uint32_t seed = 7u;
	for (int i = 0; i < 4000; i++) {
		const cs::vec3 a(frand(seed, 20, 36), frand(seed, 60, 105), frand(seed, 28, 44));
		const cs::vec3 b(frand(seed, 20, 36), frand(seed, 60, 105), frand(seed, 28, 44));
		const float da = ev.conifer_trunk_sdf(a, c), db = ev.conifer_trunk_sdf(b, c);
		CHECK(std::fabs(da - db) <= cs::length(a - b) * 1.0001f + 1e-4f);
	}
}

TEST_CASE("the record round-trips every field a scatter or imposter reads") {
	cs::Eval ev;
	const auto cp = params(ev);
	const auto c = ev.conifer_at(cs::ivec2(-12, 9), cs::vec2(-90.5f, 75.25f), 140.0f, 1.1f, cp);
	const auto u = ev.conifer_unpack(ev.conifer_pack_a(c), ev.conifer_pack_b(c));
	CHECK(u.foot.x == c.foot.x);
	CHECK(u.foot.y == c.foot.y);
	CHECK(u.foot.z == c.foot.z);
	CHECK(u.hash == c.hash);
	CHECK(u.height == c.height);
	CHECK(u.crown_base == c.crown_base);
	CHECK(u.R == c.R);
	CHECK(u.tiers == c.tiers);
	CHECK(u.droop == doctest::Approx(c.droop).epsilon(1e-5));
}

// Ground configurations: flat, a gentle plane trees stand on, and the terraced wall.
void configure(cs::Eval &ev, int k) {
	ev.terraced = k == 2;
	ev.slope_x = k == 1 ? 1.0f : 0.0f;
}

TEST_CASE("the walk never overstates the trunk distance, and is exact inside d_safe") {
	cs::Eval ev;
	const auto cp = params(ev);
	for (int k = 0; k < 3; k++) {
		configure(ev, k);
		uint32_t seed = 11u;
		for (int i = 0; i < 3000; i++) {
			const float x = frand(seed, -60, 60), z = frand(seed, -60, 60);
			const float ground_y = kSurfaceY + ev.conifer_ground(cs::vec2(x, z)).x;
			const cs::vec3 p(x, ground_y + frand(seed, -2, 45), z);
			const float truth = brute_trunk_distance(ev, cp, p, 4);
			const auto hit = ev.conifer_walk(p, ground_y, cp, 1.0e30f, false);
			CHECK(hit.d <= truth + 1e-4f);
			if (truth < ev.conifer_d_safe(cp)) CHECK(hit.d == doctest::Approx(truth).epsilon(1e-5));
		}
	}
}

// Review Focus 1: a trunk on a ledge above the point, on walls up to 8.5 (the field bound is
// 8.6), never makes the stage's answer overstate the distance by more than the field's own L.
TEST_CASE("the far bound and the combined field never overstate, even under a cliff-top trunk") {
	cs::Eval ev;
	const auto cp = params(ev);
	int trees_near = 0;
	for (int k = 0; k < 3; k++) {
		configure(ev, k);
		uint32_t seed = 23u;
		for (int i = 0; i < 3000; i++) {
			const float x = frand(seed, -40, 40), z = frand(seed, -40, 40);
			const float ground_y = kSurfaceY + ev.conifer_ground(cs::vec2(x, z)).x;
			const float h = frand(seed, -2, 400);
			const cs::vec3 p(x, ground_y + h, z);
			// 12 cells = 96 m of neighbours: every trunk a 400 m-high point could be near.
			const float truth = brute_trunk_distance(ev, cp, p, 12);
			trees_near += truth < 60.0f;
			CHECK(ev.conifer_far_bound(h, cp) <= truth + 1e-3f);
			const float f = h; // the plane's field value at p
			const auto hit = ev.conifer_field(p, f, ground_y, cp, false, 0.06f);
			CHECK(hit.d <= f + 1e-6f);
			CHECK(hit.d / 8.6f <= std::fmin(truth, f) + 1e-3f);
		}
	}
	CHECK(trees_near > 1000); // the cases above actually had trunks to overstate
}

TEST_CASE("the forest disc covers the ground under a present crown and nothing far from one") {
	cs::Eval ev;
	const auto cp = params(ev);
	int at_feet = 0, discs = 0;
	for (int cx = 0; cx < 30; cx++) {
		const cs::ivec2 cell(cx, 2);
		if (!ev.conifer_cell_gate(cell, cp)) continue;
		const cs::vec2 xz = ev.conifer_cell_xz(cell, cp);
		const cs::vec3 p(xz.x + 0.5f, kSurfaceY + 10.0f - 0.1f, xz.y);
		at_feet++;
		discs += ev.conifer_walk(p, kSurfaceY + 10.0f, cp, 0.06f, true).forest;
	}
	CHECK(at_feet > 0);
	CHECK(discs == at_feet);
}

TEST_CASE("the imposter ray hits the crown from the side and from above, and misses beside it") {
	cs::Eval ev;
	const auto cp = params(ev);
	const auto c = ev.conifer_at(cs::ivec2(0, 0), cs::vec2(0.0f, 0.0f), 60.0f, 0.2f, cp);
	const float mid_y = c.foot.y + c.crown_base + 0.3f * (c.height - c.crown_base);
	const cs::vec3 side_o(-300.0f, mid_y, 0.0f), side_d(1.0f, 0.0f, 0.0f);
	const float t = ev.conifer_ray_hit(c, side_o, side_d);
	REQUIRE(t > 0.0f);
	CHECK(ev.conifer_inside(c, side_o + side_d * t));
	CHECK(t < 300.0f); // in front of the axis
	const cs::vec3 top_o(0.0f, c.foot.y + c.height + 200.0f, 0.0f), down(0.0f, -1.0f, 0.0f);
	CHECK(ev.conifer_ray_hit(c, top_o, down) > 0.0f);
	const cs::vec3 miss_o(-300.0f, mid_y, c.R * 1.2f + 0.5f);
	CHECK(ev.conifer_ray_hit(c, miss_o, side_d) < 0.0f);
}
