#include <doctest/doctest.h>
#include <cmath>
#include <algorithm>

// Executes the ACTUAL shaders/leaf_normal.glslh natively, exactly as test_tree_shader.cpp
// executes tree.glslh. The shim supplies GLSL scalar/vector operations only; none of the leaf maths is
// reimplemented here, so this cannot drift from the shader.
namespace {
namespace leaf_shader {
template<class T> struct V2 {
	T x, y;
	V2() : x(0), y(0) {}
	V2(T a, T b) : x(a), y(b) {}
};
template<class T> struct V3 {
	T x, y, z;
	V3() : x(0), y(0), z(0) {}
	V3(T a, T b, T c) : x(a), y(b), z(c) {}
};
using vec2 = V2<float>;
using vec3 = V3<float>;

vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross(vec3 a, vec3 b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(vec3 a) { return std::sqrt(dot(a, a)); }
vec3 normalize(vec3 a) { return a / length(a); }
float abs(float v) { return std::fabs(v); }

#include "../../shaders/leaf_normal.glslh"
} // namespace leaf_shader
namespace ls = leaf_shader;

// A unit vector on the crown sphere: `elev` above the horizon, `azim` around it.
ls::vec3 crown_normal(float elev, float azim) {
	return {std::cos(elev) * std::cos(azim), std::sin(elev), std::cos(elev) * std::sin(azim)};
}

constexpr float kPi = 3.14159265358979f;
// The four card corners leaf.vert.glsl draws, plus the centre.
const ls::vec2 kCorners[5] = {{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}, {0.f, 0.f}};
} // namespace

// THE regression. leaf.vert.glsl used to turn the card normal toward the viewer
// (`if (dot(sn, to_cam) < 0.0) sn = -sn;`), which negates n.y -- the exact value
// leaf.frag.glsl mixes kUnder/kTop with. A clump crossing dot == 0, by the camera orbiting
// or by the wind swaying the card, swapped colour in one frame.
//
// leaf_card_normal() spreads the normal in a basis PERPENDICULAR to n, so dot(result, n) is
// dot(n, n) = 1 before renormalising: positive for every corner and every ratio. The spread
// can bend the normal arbitrarily far but can never invert it.
TEST_CASE("leaf_card_normal never inverts the transferred crown normal") {
	for (int e = -8; e <= 8; e++) {
		for (int a = 0; a < 16; a++) {
			const ls::vec3 n = crown_normal(float(e) / 8.f * kPi * 0.5f, float(a) / 16.f * 2.f * kPi);
			for (const ls::vec2 &q : kCorners) {
				// ratio is clamped to 0..1 by leaf_scatter.comp.glsl; walk the whole range
				// plus the degenerate end, because the invariant must not depend on it.
				for (float ratio : {0.0f, 0.15f, 0.5f, 1.0f}) {
					const ls::vec3 sn = ls::leaf_card_normal(n, q, ratio);
					CHECK(ls::dot(sn, n) > 0.0f);
					CHECK(ls::length(sn) == doctest::Approx(1.0f));
				}
			}
		}
	}
}

// The same bug seen from the other side: the colour the fragment picks is a CONTINUOUS
// function of where the clump sits on the crown. Assert the secants directly -- consecutive
// samples along a great circle move by no more than a small multiple of the step -- rather
// than any gradient-norm proxy, which can read high at a crease that is not a jump.
TEST_CASE("leaf_card_normal y varies continuously around the crown") {
	const int kSteps = 720;
	const float kStep = 2.f * kPi / float(kSteps);
	for (const ls::vec2 &q : kCorners) {
		float worst = 0.0f;
		for (int i = 0; i < kSteps; i++) {
			// A great circle through both poles, so this sweep crosses the |n.y| < 0.99
			// basis switch in leaf_card_normal() twice -- the one place a discontinuity
			// could hide now that the view-dependent flip is gone.
			const ls::vec3 a = crown_normal(float(i) * kStep, 0.7f);
			const ls::vec3 b = crown_normal(float(i + 1) * kStep, 0.7f);
			worst = std::max(worst,
					std::fabs(ls::leaf_card_normal(a, q, 0.15f).y - ls::leaf_card_normal(b, q, 0.15f).y));
		}
		// A flip would land near 2.0 (y to -y); a basis switch that leaked would land at the
		// card's own extent. 8x the step leaves room for the corner's lever arm and nothing else.
		CHECK(worst < 8.0f * kStep);
	}
}
