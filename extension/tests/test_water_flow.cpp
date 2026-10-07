#include <doctest/doctest.h>
#include <cmath>

// Execute the ACTUAL water flow GLSL in a native test, the way test_grass_tilt_shader.cpp
// executes the blade tilt. The shim supplies only GLSL scalar/vector operations; none of the
// flow is reimplemented here, so this cannot drift from the shader.
//
// The property under test is the user's requirement: water always LOOKS like it flows
// downward (docs/superpowers/specs/2026-10-06-water-voxels-design.md §4). Every pattern on a
// sloped or vertical face slides toward -y, and nothing on a flat top can fall.
namespace {
namespace shader {
struct vec2 {
	float x, y;
	explicit vec2(float a) : x(a), y(a) {}
	vec2(float a, float b) : x(a), y(b) {}
};
vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
vec2 operator*(float a, vec2 b) { return b * a; }
vec2 operator/(vec2 a, float b) { return {a.x / b, a.y / b}; }
struct vec3 {
	float x, y, z;
	explicit vec3(float a) : x(a), y(a), z(a) {}
	vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};
vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
vec3 &operator*=(vec3 &a, vec3 b) { a = a * b; return a; }
vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
vec3 abs(vec3 a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }
float max(float a, float b) { return a > b ? a : b; }
#include "../../shaders/water_flow.glslh"
} // namespace shader
} // namespace

using shader::vec2;
using shader::vec3;

// The world y at which the side texel whose uv.y is `c` sits at time t. uv.y is affine in y,
// so two samples give the line and its root.
static float side_feature_y(float c, float t, float speed, int layer) {
	const float u0 = shader::water_side_uv(vec2(1.0f, 0.0f), t, speed, layer).y;
	const float u1 = shader::water_side_uv(vec2(1.0f, 1.0f), t, speed, layer).y;
	return (c - u0) / (u1 - u0);
}

TEST_CASE("every side layer slides its pattern down the face over time") {
	for (int layer = 0; layer < 2; layer++)
		for (float speed : {0.1f, 0.6f, 2.0f}) {
			const float y0 = side_feature_y(0.37f, 0.0f, speed, layer);
			const float y1 = side_feature_y(0.37f, 1.0f, speed, layer);
			CHECK(y1 < y0); // down, never up
			const float expect = speed * (layer == 0 ? 1.0f : shader::WATER_SIDE_SPEED_B);
			CHECK(y0 - y1 == doctest::Approx(expect).epsilon(1e-3));
			// The horizontal never moves: water on a wall falls straight down.
			CHECK(shader::water_side_uv(vec2(2.5f, 0.0f), 0.0f, speed, layer).x ==
					doctest::Approx(shader::water_side_uv(vec2(2.5f, 0.0f), 7.0f, speed, layer).x));
		}
}

TEST_CASE("a zero flow speed freezes the side pattern") {
	for (int layer = 0; layer < 2; layer++) {
		const vec2 a = shader::water_side_uv(vec2(1.0f, 3.0f), 0.0f, 0.0f, layer);
		const vec2 b = shader::water_side_uv(vec2(1.0f, 3.0f), 50.0f, 0.0f, layer);
		CHECK(a.x == doctest::Approx(b.x));
		CHECK(a.y == doctest::Approx(b.y));
	}
}

TEST_CASE("the top layers drift sideways, detuned in scale and direction") {
	for (int layer = 0; layer < 2; layer++) {
		const vec2 d = shader::water_top_dir(layer);
		CHECK(std::sqrt(d.x * d.x + d.y * d.y) == doctest::Approx(1.0f).epsilon(1e-3));
		const float tile = layer == 0 ? shader::WATER_TOP_TILE_A : shader::WATER_TOP_TILE_B;
		const float speed = layer == 0 ? shader::WATER_TOP_SPEED_A : shader::WATER_TOP_SPEED_B;
		const vec2 a = shader::water_top_uv(vec2(0.0f, 0.0f), 0.0f, layer);
		const vec2 b = shader::water_top_uv(vec2(0.0f, 0.0f), 10.0f, layer);
		// A feature moves along +dir at `speed`: the uv at a fixed point moves along -dir.
		CHECK((b.x - a.x) * tile == doctest::Approx(-d.x * speed * 10.0f).epsilon(1e-3));
		CHECK((b.y - a.y) * tile == doctest::Approx(-d.y * speed * 10.0f).epsilon(1e-3));
	}
	const vec2 a = shader::water_top_dir(0), b = shader::water_top_dir(1);
	const float deg = std::acos(a.x * b.x + a.y * b.y) * 57.29578f;
	CHECK(deg > 100.0f);
	CHECK(deg < 120.0f);
	// The tile ratio is not near a whole number, so the two periods never line up.
	const float ratio = shader::WATER_TOP_TILE_A / shader::WATER_TOP_TILE_B;
	CHECK(std::fabs(ratio - std::round(ratio)) > 0.2f);
}

TEST_CASE("triplanar weights put a flat top on the top projection and a wall on a side") {
	const vec3 top = shader::water_weights(vec3(0.0f, 1.0f, 0.0f));
	CHECK(top.y == doctest::Approx(1.0f));
	CHECK(top.x == doctest::Approx(0.0f));
	CHECK(top.z == doctest::Approx(0.0f));
	const vec3 wall = shader::water_weights(vec3(0.0f, 0.0f, -1.0f));
	CHECK(wall.z == doctest::Approx(1.0f));
	CHECK(wall.y == doctest::Approx(0.0f));
	// A 45-degree slope is shared between the top's drift and a side's fall, summing to one.
	const float h = std::sqrt(0.5f);
	const vec3 slope = shader::water_weights(vec3(h, h, 0.0f));
	CHECK(slope.x == doctest::Approx(0.5f));
	CHECK(slope.y == doctest::Approx(0.5f));
	CHECK(slope.x + slope.y + slope.z == doctest::Approx(1.0f));
}

TEST_CASE("the uv scales are the derivatives of the uv maps") {
	for (int layer = 0; layer < 2; layer++) {
		const vec2 s = shader::water_side_scale(layer);
		const vec2 a = shader::water_side_uv(vec2(0.0f, 0.0f), 2.0f, 0.6f, layer);
		const vec2 b = shader::water_side_uv(vec2(1.0f, 1.0f), 2.0f, 0.6f, layer);
		CHECK(b.x - a.x == doctest::Approx(s.x));
		CHECK(b.y - a.y == doctest::Approx(s.y));
		const vec2 ts = shader::water_top_scale(layer);
		const vec2 c = shader::water_top_uv(vec2(0.0f, 0.0f), 2.0f, layer);
		const vec2 e = shader::water_top_uv(vec2(1.0f, 1.0f), 2.0f, layer);
		CHECK(e.x - c.x == doctest::Approx(ts.x));
		CHECK(e.y - c.y == doctest::Approx(ts.y));
	}
}
