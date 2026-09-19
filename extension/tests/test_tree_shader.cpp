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
using std::cos;
using std::sin;
using std::sqrt;
#include "../../shaders/tree.glslh"
} // namespace tree_shader
namespace ts = tree_shader;

ts::TreeParams params() {
	ts::TreeParams tp;
	tp.cell = 14.0f;
	tp.density = 0.55f;
	tp.trunk_height = 9.0f;
	tp.trunk_radius = 0.30f;
	tp.branch_radius_min = 0.10f;
	tp.crown_radius = 4.0f;
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
			if (ts::tree_cell_present({x, z}, tp, 0.0f)) present++;
	const float frac = float(present) / float(total);
	CHECK(frac > 0.40f);
	CHECK(frac < 0.70f);
}

TEST_CASE("zero density places no trees and slope rejects every cell") {
	ts::TreeParams tp = params();
	tp.density = 0.0f;
	for (int x = -20; x <= 20; x++)
		for (int z = -20; z <= 20; z++)
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, 0.0f));

	tp = params();
	for (int x = -20; x <= 20; x++)
		for (int z = -20; z <= 20; z++)
			CHECK_FALSE(ts::tree_cell_present({x, z}, tp, tp.max_slope + 0.01f));
}

TEST_CASE("every lobe sits inside the crown bounding sphere") {
	const ts::TreeParams tp = params();
	int checked = 0;
	for (int x = -15; x <= 15; x++) {
		for (int z = -15; z <= 15; z++) {
			if (!ts::tree_cell_present({x, z}, tp, 0.0f)) continue;
			const ts::Tree t = ts::tree_at({x, z}, tp, 51.2f, 0.0f);
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
			if (ts::tree_cell_present({x, z}, tp, 0.0f))
				CHECK(ts::tree_at({x, z}, tp, 51.2f, 0.0f).crown_r <= tp.crown_radius + 1e-4f);
}

TEST_CASE("a tree sits on the ground height it was given") {
	const ts::TreeParams tp = params();
	for (float g : {0.0f, 51.2f, -18.5f}) {
		for (int x = -8; x <= 8; x++)
			for (int z = -8; z <= 8; z++)
				if (ts::tree_cell_present({x, z}, tp, 0.0f))
					CHECK(ts::tree_at({x, z}, tp, g, 0.0f).base.y == doctest::Approx(g));
	}
}
