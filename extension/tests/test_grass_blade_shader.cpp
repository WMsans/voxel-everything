#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>

// Executes the ACTUAL blade GLSL (shaders/grass_blade.glslh) natively, the same way
// test_grass_tilt_shader.cpp executes the camera tilt. The shim supplies GLSL scalar/vector
// operations only; none of the blade maths is reimplemented here.
namespace {
namespace blade_shader {
using uint = uint32_t;
template<class T> struct V2 {
	T x, y;
	explicit V2(T a) : x(a), y(a) {}
	V2(T a, T b) : x(a), y(b) {}
};
template<class T> struct V3 {
	T x, y, z;
	explicit V3(T a) : x(a), y(a), z(a) {}
	V3(T a, T b, T c) : x(a), y(b), z(c) {}
};
using vec2 = V2<float>;
using vec3 = V3<float>;
vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
float length(vec3 a) { return std::sqrt(dot(a, a)); }
float length(vec2 a) { return std::sqrt(dot(a, a)); }
vec3 normalize(vec3 a) { return a / length(a); }
vec2 normalize(vec2 a) { float l = length(a); return {a.x / l, a.y / l}; }
float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
float max(float a, float b) { return a > b ? a : b; }
float round(float v) { return std::round(v); }
using std::cos;
using std::sin;
using std::sqrt;
#include "../../shaders/grass_blade.glslh"
} // namespace blade_shader
namespace bs = blade_shader;

// Numerical arc length of the blade profile, by fine polyline.
float profile_length(float height, float bend) {
	float len = 0.0f;
	bs::vec2 prev = bs::grass_blade_point(0.0f, height, bend);
	for (int i = 1; i <= 400; i++) {
		bs::vec2 p = bs::grass_blade_point(float(i) / 400.0f, height, bend);
		len += bs::length(p - prev);
		prev = p;
	}
	return len;
}
} // namespace

TEST_CASE("the sun byte and the ground normal share one float without loss") {
	for (uint32_t oct : {0u, 0x1234u, 0x80FFu, 0xFFFFu}) {
		for (float sun : {0.0f, 0.25f, 0.5f, 1.0f}) {
			const float packed = bs::grass_pack_ground(oct, sun);
			CHECK(bs::grass_ground_oct(packed) == oct);
			CHECK(bs::grass_ground_sun(packed) == doctest::Approx(sun).epsilon(1.0 / 255.0));
			// The whole point of stopping at 24 bits: a float holds every integer below 2^24.
			CHECK(packed < 16777216.0f);
		}
	}
	// Out-of-range visibility is clamped rather than wrapping into the normal's bits.
	CHECK(bs::grass_ground_sun(bs::grass_pack_ground(0xFFFFu, 3.0f)) == doctest::Approx(1.0f));
	CHECK(bs::grass_ground_oct(bs::grass_pack_ground(0xFFFFu, 3.0f)) == 0xFFFFu);
}

TEST_CASE("an unbent blade stands straight up to its full height") {
	const bs::vec2 tip = bs::grass_blade_point(1.0f, 0.9f, 0.0f);
	CHECK(tip.x == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(tip.y == doctest::Approx(0.9f).epsilon(1e-3));
	const bs::vec2 root = bs::grass_blade_point(0.0f, 0.9f, 0.0f);
	CHECK(root.x == doctest::Approx(0.0f));
	CHECK(root.y == doctest::Approx(0.0f));
}

// The stiffness bug: wind used to SLIDE the tip sideways, so a gust stretched the blade
// rather than bending it. A bend must keep the blade's length.
TEST_CASE("bending keeps the blade's length") {
	for (float bend : {-0.8f, 0.0f, 0.3f, 0.8f, 1.2f, 1.5f}) {
		CAPTURE(bend);
		CHECK(profile_length(1.0f, bend) == doctest::Approx(1.0f).epsilon(0.005));
	}
}

TEST_CASE("a harder bend lowers the tip and carries it further along the arc") {
	float last_y = 2.0f, last_x = -1.0f;
	for (float bend : {0.0f, 0.4f, 0.8f, 1.2f}) {
		CAPTURE(bend);
		const bs::vec2 tip = bs::grass_blade_point(1.0f, 1.0f, bend);
		CHECK(tip.y < last_y);
		CHECK(tip.x > last_x);
		last_y = tip.y;
		last_x = tip.x;
	}
	// A negative bend leans the other way.
	CHECK(bs::grass_blade_point(1.0f, 1.0f, -0.6f).x < 0.0f);
}

TEST_CASE("the blade leaves the ground along its growth axis and the tangent is unit") {
	for (float bend : {0.0f, 0.7f, 1.4f}) {
		CAPTURE(bend);
		const bs::vec2 t0 = bs::grass_blade_tangent(0.0f, bend);
		CHECK(bs::length(t0) == doctest::Approx(1.0f).epsilon(1e-3));
		// The base is planted: the first segment is only a sixth of the bend off vertical.
		CHECK(t0.y >= std::cos(bend * 0.5f) - 1e-4f);
		const bs::vec2 t1 = bs::grass_blade_tangent(1.0f, bend);
		CHECK(bs::length(t1) == doctest::Approx(1.0f).epsilon(1e-3));
	}
}

TEST_CASE("wind bend adds a signed sway to the static curve and is bounded") {
	const float rest = bs::grass_wind_bend(0.45f, 0.0f, 1.0f);
	CHECK(bs::grass_wind_bend(0.45f, 0.3f, 1.0f) > rest);
	CHECK(bs::grass_wind_bend(0.45f, -0.3f, 1.0f) < rest);
	// A gust pushing back can carry the blade past upright.
	CHECK(bs::grass_wind_bend(0.1f, -1.0f, 1.0f) < 0.0f);
	CHECK(bs::grass_wind_bend(2.0f, 50.0f, 1.0f) <= 1.5f);
	CHECK(bs::grass_wind_bend(0.0f, -50.0f, 1.0f) >= -1.2f);
}

TEST_CASE("with no blend the shading normal is exactly the ground normal") {
	const bs::vec3 ground(0.0f, 1.0f, 0.0f);
	const bs::vec3 face(1.0f, 0.0f, 0.0f);
	const bs::vec3 side(0.0f, 0.0f, 1.0f);
	const bs::vec3 n = bs::grass_shading_normal(ground, face, side, 1.0f, 0.5f, 0.0f);
	CHECK(n.x == doctest::Approx(0.0f));
	CHECK(n.y == doctest::Approx(1.0f));
	CHECK(n.z == doctest::Approx(0.0f));
}

// The black-grass bug the ground-normal commit fixed: a blade normal can never be allowed to
// aim into the terrain, whatever the face normal and blend.
TEST_CASE("the shading normal never points into the ground") {
	const bs::vec3 ground = bs::normalize(bs::vec3(0.2f, 1.0f, 0.1f));
	const bs::vec3 side(0.0f, 0.0f, 1.0f);
	for (float fx : {-1.0f, 0.0f, 1.0f}) {
		for (float fy : {-1.0f, -0.2f, 0.3f}) {
			for (float u : {-1.0f, 0.0f, 1.0f}) {
				const bs::vec3 face = bs::normalize(bs::vec3(fx, fy, 0.4f));
				const bs::vec3 n = bs::grass_shading_normal(ground, face, side, u, 0.0f, 1.0f);
				CAPTURE(fx); CAPTURE(fy); CAPTURE(u);
				CHECK(bs::length(n) == doctest::Approx(1.0f).epsilon(1e-3));
				CHECK(bs::dot(n, ground) >= 0.249f);
			}
		}
	}
}

TEST_CASE("the width coordinate rounds the blade and the tip leans back to the ground normal") {
	const bs::vec3 ground(0.0f, 1.0f, 0.0f);
	const bs::vec3 face(1.0f, 0.0f, 0.0f);
	const bs::vec3 side(0.0f, 0.0f, 1.0f);
	const bs::vec3 left = bs::grass_shading_normal(ground, face, side, -1.0f, 0.0f, 0.6f);
	const bs::vec3 right = bs::grass_shading_normal(ground, face, side, 1.0f, 0.0f, 0.6f);
	CHECK(bs::dot(right, side) > 0.0f);
	CHECK(bs::dot(left, side) < 0.0f);
	const bs::vec3 root = bs::grass_shading_normal(ground, face, side, 0.0f, 0.0f, 0.6f);
	const bs::vec3 tip = bs::grass_shading_normal(ground, face, side, 0.0f, 1.0f, 0.6f);
	CHECK(bs::dot(tip, ground) > bs::dot(root, ground));
}

TEST_CASE("canopy self-shading dims the root and leaves the tip fully sunlit") {
	CHECK(bs::grass_sun_term(1.0f, 1.0f) == doctest::Approx(1.0f));
	CHECK(bs::grass_sun_term(1.0f, 0.0f) < 0.5f);
	CHECK(bs::grass_sun_term(1.0f, 0.0f) > 0.0f);
	CHECK(bs::grass_sun_term(0.0f, 1.0f) == doctest::Approx(0.0f));
	float last = -1.0f;
	for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
		CHECK(bs::grass_sun_term(1.0f, t) > last);
		last = bs::grass_sun_term(1.0f, t);
	}
	// It scales the terrain visibility; it never lightens a shadowed blade.
	CHECK(bs::grass_sun_term(0.4f, 1.0f) == doctest::Approx(0.4f));
}
