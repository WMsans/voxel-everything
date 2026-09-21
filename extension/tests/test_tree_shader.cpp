#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <initializer_list>

// Executes the ACTUAL shaders/tree.glslh natively, exactly as
// test_grass_blade_shader.cpp executes the blade maths. The shim supplies GLSL
// scalar/vector operations only; none of the tree maths is reimplemented here.
namespace {
namespace tree_shader {
using uint = uint32_t;
template<class T> struct V2 {
	T x, y;
	V2() : x(0), y(0) {}
	explicit V2(T a) : x(a), y(a) {}
	V2(T a, T b) : x(a), y(b) {}
};
template<class T> struct V3 {
	T x, y, z;
	V3() : x(0), y(0), z(0) {}
	explicit V3(T a) : x(a), y(a), z(a) {}
	V3(T a, T b, T c) : x(a), y(b), z(c) {}
};
using vec2 = V2<float>;
using vec3 = V3<float>;
using ivec2 = V2<int>;
using uvec2 = V2<uint>;
vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
ivec2 operator+(ivec2 a, ivec2 b) { return {a.x + b.x, a.y + b.y}; }
vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
float length(vec3 a) { return std::sqrt(dot(a, a)); }
float length(vec2 a) { return std::sqrt(dot(a, a)); }
vec3 normalize(vec3 a) { return a / length(a); }
float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
float min(float a, float b) { return a < b ? a : b; }
float max(float a, float b) { return a > b ? a : b; }
float floor(float v) { return std::floor(v); }
float abs(float v) { return std::fabs(v); }
float sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
using std::cos;
using std::sin;
using std::sqrt;
#include "../../shaders/tree.glslh"
} // namespace tree_shader
namespace ts = tree_shader;

ts::TreeParams params() {
	ts::TreeParams tp;
	tp.cell = 18.0f;
	tp.density = 0.55f;
	tp.trunk_height = 8.0f;
	tp.trunk_radius = 0.55f;
	tp.branch_radius_min = 0.14f;
	tp.crown_radius = 6.0f;
	tp.max_slope = 0.6f;
	return tp;
}
} // namespace

TEST_CASE("tree placement is deterministic per cell") {
	const ts::TreeParams tp = params();
	for (int x : {-9, 0, 3, 77}) {
		for (int z : {-4, 0, 11, 250}) {
			const ts::vec2 a = ts::tree_cell_xz({x, z}, tp);
			const ts::vec2 b = ts::tree_cell_xz({x, z}, tp);
			CHECK(a.x == b.x);
			CHECK(a.y == b.y);
		}
	}
}

TEST_CASE("a tree stays inside its own cell's jitter box") {
	const ts::TreeParams tp = params();
	const float half = TREE_JITTER * tp.cell;
	for (int x = -20; x <= 20; x++) {
		for (int z = -20; z <= 20; z++) {
			const ts::vec2 p = ts::tree_cell_xz({x, z}, tp);
			const float cx = (float(x) + 0.5f) * tp.cell;
			const float cz = (float(z) + 0.5f) * tp.cell;
			CHECK(std::fabs(p.x - cx) <= half + 1e-4f);
			CHECK(std::fabs(p.y - cz) <= half + 1e-4f);
		}
	}
}

TEST_CASE("density gates roughly the requested fraction of cells") {
	ts::TreeParams tp = params();
	int present = 0, total = 0;
	for (int x = -40; x <= 40; x++)
		for (int z = -40; z <= 40; z++, total++)
			if (ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f)) present++;
	const float frac = float(present) / float(total);
	CHECK(frac > 0.40f);
	CHECK(frac < 0.70f);
}

TEST_CASE("zero density places no trees and slope rejects every cell") {
	ts::TreeParams tp = params();
	tp.density = 0.0f;
	for (int x = -20; x <= 20; x++)
		for (int z = -20; z <= 20; z++)
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f));

	tp = params();
	for (int x = -20; x <= 20; x++)
		for (int z = -20; z <= 20; z++)
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, 2.0f, tp.max_slope + 0.01f));
}

// SPEC §4, the height band. Final review wave: the shipped gate used to consult only the
// slope, and the probe over the shipped pipeline counted 64,552 placing cells in +-3000 m
// of which 32,474 stood on dirt and 31,207 on rock. The band mirrors stage_height_bands:
// grass is 1 < h <= 4, everything else rejects.
TEST_CASE("the height band rejects dirt, rock and their edges") {
	const ts::TreeParams tp = params();
	int checked = 0;
	for (int x = -40; x <= 40; x++)
		for (int z = -40; z <= 40; z++) {
			if (!ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f)) continue;
			checked++;
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, 1.0f, 0.0f)); // dirt side: grass needs h > 1
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, 0.5f, 0.0f)); // dirt
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, -30.0f, 0.0f)); // deep dirt
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, 4.001f, 0.0f)); // rock
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, 250.0f, 0.0f)); // high rock (snow world)
			CHECK(ts::tree_cell_present({x, z}, tp, 4.0f, 0.0f)); // the rock side belongs to grass
			CHECK(ts::tree_cell_present({x, z}, tp, 1.001f, 0.0f)); // just inside the dirt side
		}
	CHECK(checked > 100); // the band must reject OUT of band, not everything
}

TEST_CASE("every lobe sits inside the crown bounding sphere") {
	const ts::TreeParams tp = params();
	int checked = 0;
	for (int x = -15; x <= 15; x++) {
		for (int z = -15; z <= 15; z++) {
			if (!ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f)) continue;
			const ts::Tree t = ts::tree_at({x, z}, tp, 51.2f, 2.0f, 0.0f);
			REQUIRE(t.present);
			for (int i = 0; i < TREE_LOBES; i++) {
				const ts::vec3 c = ts::tree_lobe(t, tp, i);
				const float r = ts::tree_lobe_radius(t, tp, i);
				CHECK(ts::length(c - t.crown) + r <= t.crown_r + 1e-3f);
				CHECK(r > 0.0f);
			}
			checked++;
		}
	}
	CHECK(checked > 100); // the sweep must actually have found trees
}

TEST_CASE("the crown bound never exceeds the declared crown radius") {
	const ts::TreeParams tp = params();
	for (int x = -15; x <= 15; x++)
		for (int z = -15; z <= 15; z++)
			if (ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f))
				CHECK(ts::tree_at({x, z}, tp, 51.2f, 2.0f, 0.0f).crown_r <= tp.crown_radius + 1e-4f);
}

TEST_CASE("a tree sits on the ground height it was given") {
	const ts::TreeParams tp = params();
	for (float g : {0.0f, 51.2f, -18.5f}) {
		for (int x = -8; x <= 8; x++)
			for (int z = -8; z <= 8; z++)
				if (ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f))
					CHECK(ts::tree_at({x, z}, tp, g, 2.0f, 0.0f).base.y == doctest::Approx(g));
	}
}

namespace {
// Flat ground at y = 51.2 and zero slope everywhere, so this exercises tree.glslh alone.
constexpr float kFlatGround = 51.2f;

float sweep(ts::vec3 p, const ts::TreeParams &tp, int ring) {
	const int cx = int(std::floor(p.x / tp.cell));
	const int cz = int(std::floor(p.z / tp.cell));
	float d = 1.0e9f;
	for (int dz = -ring; dz <= ring; dz++) {
		for (int dx = -ring; dx <= ring; dx++) {
			const ts::Tree t = ts::tree_at({cx + dx, cz + dz}, tp, kFlatGround, 2.0f, 0.0f);
			if (!t.present) continue;
			d = std::min(d, ts::tree_skeleton_sdf(p, t, tp));
		}
	}
	return d;
}
} // namespace

TEST_CASE("the 3x3 clamped sweep never overestimates the true distance") {
	const ts::TreeParams tp = params();
	uint32_t s = 20260918u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	for (int i = 0; i < 20000; i++) {
		const ts::vec3 p(next(-200.0f, 200.0f), next(kFlatGround - 3.0f, kFlatGround + 14.0f),
				next(-200.0f, 200.0f));
		const float got = std::min(sweep(p, tp, 1), ts::tree_d_safe(tp));
		const float truth = sweep(p, tp, 4); // four rings: far wider than any tree can reach
		CHECK(got <= truth + 1e-3f);
	}
}

TEST_CASE("D_safe is derived from the params, not a constant") {
	ts::TreeParams tp = params();
	const float a = ts::tree_d_safe(tp);
	tp.crown_radius = 8.0f; // a bigger crown must TIGHTEN the clamp
	CHECK(ts::tree_d_safe(tp) < a);
	tp = params();
	tp.cell = 28.0f;        // a bigger cell must LOOSEN it
	CHECK(ts::tree_d_safe(tp) > a);
	CHECK(a > 0.0f);
}

// Lipschitz is tested by SECANTS, not by the norm of three central differences. The old
// probe did the latter and it is unsound at a crease: for a genuinely 1-Lipschitz field
// each per-axis difference is <= 1 by construction, but the NORM of three of them can reach
// sqrt(3) wherever two pieces tie and the field is not differentiable. That probe held only
// because the old skeleton's creases happened to be shallow; it flagged 2 points in 20000
// on the reshaped trunk, both of them ties (1.4692 vs 1.4744) rather than real violations.
// |f(a) - f(b)| <= |a - b| is the property the raymarcher actually depends on, and it is
// exact at creases, so that is what this asserts.
TEST_CASE("the skeleton SDF is 1-Lipschitz") {
	const ts::TreeParams tp = params();
	const ts::Tree t = ts::tree_at({0, 0}, tp, kFlatGround, 2.0f, 0.0f);
	uint32_t s = 7771u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	for (int i = 0; i < 40000; i++) {
		const ts::vec3 a(next(-12.0f, 12.0f), next(kFlatGround - 2.0f, kFlatGround + 16.0f),
				next(-12.0f, 12.0f));
		// Both a far partner and a near one: the near pairs are what probe the creases.
		const float reach = (i % 2 == 0) ? 6.0f : 0.05f;
		const ts::vec3 b(a.x + next(-reach, reach), a.y + next(-reach, reach),
				a.z + next(-reach, reach));
		const float fa = ts::tree_skeleton_sdf(a, t, tp);
		const float fb = ts::tree_skeleton_sdf(b, t, tp);
		CHECK(std::fabs(fa - fb) <= ts::length(b - a) + 1e-4f);
	}
}

// The secant test above is only worth having if it fails on a field that overstates
// distance. A 1.02x scale is the mildest realistic version of that mistake -- a smooth
// minimum, or a mis-derived bound, inflates the field by about this much -- and it is far
// below the 1.02 the discarded gradient probe tolerated outright.
TEST_CASE("the Lipschitz secant test rejects a field that overstates distance") {
	const ts::TreeParams tp = params();
	const ts::Tree t = ts::tree_at({0, 0}, tp, kFlatGround, 2.0f, 0.0f);
	auto overstated = [&](ts::vec3 p) { return ts::tree_skeleton_sdf(p, t, tp) * 1.02f; };
	uint32_t s = 7771u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	int violations = 0;
	for (int i = 0; i < 40000; i++) {
		const ts::vec3 a(next(-12.0f, 12.0f), next(kFlatGround - 2.0f, kFlatGround + 16.0f),
				next(-12.0f, 12.0f));
		const ts::vec3 b(a.x + next(-6.0f, 6.0f), a.y + next(-6.0f, 6.0f), a.z + next(-6.0f, 6.0f));
		if (std::fabs(overstated(a) - overstated(b)) > ts::length(b - a) + 1e-4f) violations++;
	}
	CHECK(violations > 0);
}

// Limbs must taper from the fork to the tip. A subordinate limb whose fork radius fell
// BELOW branch_radius_min came out wider at the tip than at the trunk -- measured at
// 0.0883 -> 0.0947 m on the first cut of this reshape, which reads as a club, not a branch.
TEST_CASE("every limb tapers from its fork to its tip") {
	const ts::TreeParams tp = params();
	int checked = 0;
	for (int x = -15; x <= 15; x++) {
		for (int z = -15; z <= 15; z++) {
			if (!ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f)) continue;
			const ts::Tree t = ts::tree_at({x, z}, tp, kFlatGround, 2.0f, 0.0f);
			for (int i = 0; i < TREE_LIMBS; i++) {
				CHECK(ts::tree_limb_radius(t, tp, i) > tp.branch_radius_min);
			}
			checked++;
		}
	}
	CHECK(checked > 100);
}

TEST_CASE("the bounding capsule never rejects a point the skeleton would claim") {
	const ts::TreeParams tp = params();
	const ts::Tree t = ts::tree_at({0, 0}, tp, kFlatGround, 2.0f, 0.0f);
	uint32_t s = 4242u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	for (int i = 0; i < 20000; i++) {
		const ts::vec3 p(next(-25.0f, 25.0f), next(kFlatGround - 5.0f, kFlatGround + 25.0f),
				next(-25.0f, 25.0f));
		CHECK(ts::tree_bound(p, t) <= ts::tree_skeleton_sdf(p, t, tp) + 1e-3f);
	}
}

// --- trunk shape (2026-09-19) -------------------------------------------------------
// The old skeleton was a plumb-straight pole that fanned ten identical branches out of ONE
// point at 0.55 height -- a broom, not a tree. These four pin the structure that replaced
// it. They are shape tests, not beauty tests: each names a property the references have and
// the old skeleton provably did not.

TEST_CASE("the trunk leans off the vertical, within the bound d_safe subtracts") {
	const ts::TreeParams tp = params();
	int leaning = 0, checked = 0;
	for (int x = -15; x <= 15; x++) {
		for (int z = -15; z <= 15; z++) {
			if (!ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f)) continue;
			const ts::Tree t = ts::tree_at({x, z}, tp, kFlatGround, 2.0f, 0.0f);
			const float dx = t.crown.x - t.base.x, dz = t.crown.z - t.base.z;
			const float lean = std::sqrt(dx * dx + dz * dz);
			// The bound tree_d_safe() subtracts. Breaking it understates d_safe and tunnels.
			CHECK(lean <= TREE_MAX_LEAN * t.height + 1e-4f);
			if (lean > 0.1f) leaning++;
			checked++;
		}
	}
	CHECK(checked > 100);
	// Most trees lean visibly. A plumb-line forest is the thing being fixed.
	CHECK(leaning > checked / 2);
}

TEST_CASE("the trunk rises vertically out of the ground before it leans") {
	const ts::TreeParams tp = params();
	for (int x = -8; x <= 8; x++) {
		for (int z = -8; z <= 8; z++) {
			if (!ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f)) continue;
			const ts::Tree t = ts::tree_at({x, z}, tp, kFlatGround, 2.0f, 0.0f);
			const float dx = t.crown.x - t.base.x, dz = t.crown.z - t.base.z;
			const float lean = std::sqrt(dx * dx + dz * dz);
			if (lean < 0.2f) continue; // nothing to measure on an upright tree
			const ts::vec3 low = ts::tree_trunk_point(t, 0.1f);
			const float lx = low.x - t.base.x, lz = low.z - t.base.z;
			const float drift = std::sqrt(lx * lx + lz * lz);
			// A straight tilted pole would drift 0.1 * lean here. The curve must drift much
			// less: the foot is plumb and the lean accumulates up the trunk.
			CHECK(drift < 0.1f * lean * 0.5f);
			CHECK(ts::tree_trunk_point(t, 0.0f).y == doctest::Approx(t.base.y));
			CHECK(ts::tree_trunk_point(t, 1.0f).y == doctest::Approx(t.crown.y));
		}
	}
}

TEST_CASE("limbs leave the trunk at distinct heights") {
	const ts::TreeParams tp = params();
	int checked = 0;
	for (int x = -15; x <= 15; x++) {
		for (int z = -15; z <= 15; z++) {
			if (!ts::tree_cell_present({x, z}, tp, 2.0f, 0.0f)) continue;
			const ts::Tree t = ts::tree_at({x, z}, tp, kFlatGround, 2.0f, 0.0f);
			for (int i = 0; i < TREE_LIMBS; i++) {
				const float si = ts::tree_limb_fork(t, i);
				CHECK(si > 0.0f);
				CHECK(si < 1.0f);
				for (int j = i + 1; j < TREE_LIMBS; j++)
					// THE broom test: one shared fork point is what made it a fountain.
					CHECK(std::fabs(si - ts::tree_limb_fork(t, j)) > 0.02f);
			}
			checked++;
		}
	}
	CHECK(checked > 100);
}

TEST_CASE("the trunk is stoutest at the foot and tapers all the way up") {
	const ts::TreeParams tp = params();
	const ts::Tree t = ts::tree_at({0, 0}, tp, kFlatGround, 2.0f, 0.0f);
	// Root flare: the foot is meaningfully fatter than the bole above it.
	CHECK(ts::tree_trunk_radius(t, 0.0f) >= 1.4f * t.radius);
	CHECK(ts::tree_trunk_radius(t, 1.0f) < 0.5f * t.radius);
	float prev = ts::tree_trunk_radius(t, 0.0f);
	for (int k = 1; k <= 50; k++) {
		const float r = ts::tree_trunk_radius(t, float(k) / 50.0f);
		CHECK(r <= prev + 1e-5f); // monotone: no bulge anywhere up the trunk
		CHECK(r > 0.0f);
		prev = r;
	}
}
