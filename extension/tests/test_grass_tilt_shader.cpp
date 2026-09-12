#include <doctest/doctest.h>
#include <cmath>
#include <initializer_list>

// Execute the ACTUAL camera-tilt GLSL in a native test, the same way test_lod_quad_shader.cpp
// executes the boundary ribbon's. The shim below supplies only GLSL scalar/vector
// operations; none of the tilt is reimplemented here, so this cannot drift from the shader.
//
// The property under test is the one the function exists for: a blade is a flat card, so the
// area it presents to the camera is the cosine between its face normal and the view. Untilting
// -- the shader before this change -- that is cos(elevation), which is why a meadow reads
// full at eye level, half-covered at 60 degrees and bald straight overhead.
namespace {
// Anonymous so the shim's operators keep internal linkage: test_lod_quad_shader.cpp defines
// its own `namespace shader` in the same binary and the two must not collide at link time.
namespace shader {
template<class T> struct V3 {
	T x, y, z;
	explicit V3(T a) : x(a), y(a), z(a) {}
	V3(T a, T b, T c) : x(a), y(b), z(c) {}
	T operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
using vec3 = V3<float>;
vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
vec3 operator*(float a, vec3 b) { return b * a; }
vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross(vec3 a, vec3 b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(vec3 a) { return std::sqrt(dot(a, a)); }
vec3 normalize(vec3 a) { return a / length(a); }
float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
using std::asin;
using std::cos;
using std::sin;
#include "../../shaders/grass_tilt.glslh"
} // namespace shader
} // namespace

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;

// A unit vector from the blade towards a camera raised `elevation` above the horizon at
// `azimuth`. Azimuth is a parameter rather than a constant because the tilt must not depend
// on which way the camera is facing -- only on how far it is pitched down.
shader::vec3 to_cam_at(float elevation, float azimuth) {
	return {std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
		std::cos(elevation) * std::sin(azimuth)};
}

// One blade's frame, assembled exactly as grass.vert.glsl assembles it, so the assertions
// land on the same vectors the vertex shader will use.
struct Frame {
	shader::vec3 growth; // the axis the blade rises along, after the tilt
	shader::vec3 side;   // the width axis, billboarded perpendicular to the view
	shader::vec3 arc;    // the direction the blade's static arc travels
	shader::vec3 normal; // the card's face normal
};

Frame frame_at(float elevation, float azimuth, float lean, float tilt) {
	const shader::vec3 up(0.0f, 1.0f, 0.0f);
	const shader::vec3 to_cam = to_cam_at(elevation, azimuth);
	const shader::vec3 toward = shader::normalize(to_cam - up * shader::dot(to_cam, up));
	const shader::vec3 side = shader::normalize(shader::cross(up, toward));
	const float elev = shader::grass_camera_elevation(to_cam, up);
	const shader::vec3 growth = shader::grass_tilted_up(up, toward, elev, tilt);
	const shader::vec3 lean_dir(std::cos(lean), 0.0f, std::sin(lean));
	const shader::vec3 arc = shader::grass_plane_arc(lean_dir, growth, side);
	return {growth, side, arc, shader::normalize(shader::cross(growth, side))};
}

// Fraction of the card's face area that survives projection onto the screen.
float projected_area(const Frame &f, float elevation, float azimuth) {
	return std::abs(shader::dot(f.normal, to_cam_at(elevation, azimuth)));
}
} // namespace

TEST_CASE("the camera elevation is measured from the blade up to the camera") {
	const shader::vec3 up(0.0f, 1.0f, 0.0f);
	for (float deg : {0.0f, 10.0f, 35.26f, 60.0f, 89.9f}) {
		CHECK(shader::grass_camera_elevation(to_cam_at(deg * kDeg, 2.1f), up) ==
				doctest::Approx(deg * kDeg).epsilon(1e-4));
	}
}

// The look that already works must not move: at tilt 0 the blade is the blade the shader
// drew before this change, standing straight up on its own ground normal.
TEST_CASE("a zero tilt leaves the blade standing exactly as it was") {
	for (float deg : {0.0f, 20.0f, 45.0f, 70.0f, 89.0f}) {
		const Frame f = frame_at(deg * kDeg, 3.9f, 1.1f, 0.0f);
		CHECK(f.growth.x == doctest::Approx(0.0f));
		CHECK(f.growth.y == doctest::Approx(1.0f));
		CHECK(f.growth.z == doctest::Approx(0.0f));
		// And with it, the old area: full at eye level, cos(elevation) after that.
		CHECK(projected_area(f, deg * kDeg, 3.9f) ==
				doctest::Approx(std::cos(deg * kDeg)).epsilon(1e-4));
	}
}

// The whole point. Whatever the pitch, the tilted blade presents at least the area the
// untilted one did -- and never more than its own face, which would mean a normal pointing
// past the camera.
TEST_CASE("camera tilt never costs projected area at any pitch") {
	for (float deg : {0.0f, 15.0f, 35.26f, 45.0f, 60.0f, 75.0f, 85.0f, 89.5f}) {
		for (float tilt : {0.5f, 0.85f, 1.0f}) {
			const float elev = deg * kDeg;
			const Frame f = frame_at(elev, 1.7f, 1.1f, tilt);
			const float area = projected_area(f, elev, 1.7f);
			CHECK(area >= std::cos(elev) - 1e-4f);
			CHECK(area <= 1.0f + 1e-4f);
		}
	}
}

// A tilt of 1 is the exact fit: the blade's normal rotates all the way onto the view
// direction, so the area a level camera enjoys is held at every elevation. That is the
// definition of the technique, not an approximation of it.
TEST_CASE("a full tilt holds the eye-level area all the way to the zenith") {
	for (float deg : {0.0f, 10.0f, 30.0f, 45.0f, 60.0f, 75.0f, 85.0f, 89.5f}) {
		const float elev = deg * kDeg;
		const Frame f = frame_at(elev, 4.4f, 1.1f, 1.0f);
		CHECK(projected_area(f, elev, 4.4f) == doctest::Approx(1.0f).epsilon(1e-4));
	}
}

// Straight overhead there is no horizontal direction to lean away from -- view_h vanishes
// and the caller hands over the blade's own lean azimuth instead. The axis must still be
// finite and still lying down, never a NaN and never pinned vertical.
TEST_CASE("overhead the growth axis stays finite and lies down") {
	const float elev = 90.0f * kDeg;
	const shader::vec3 up(0.0f, 1.0f, 0.0f);
	const shader::vec3 toward = shader::normalize(shader::vec3(0.6f, 0.0f, 0.8f));
	const shader::vec3 growth = shader::grass_tilted_up(up, toward, elev, 0.85f);
	CHECK(std::isfinite(growth.x));
	CHECK(std::isfinite(growth.y));
	CHECK(std::isfinite(growth.z));
	CHECK(shader::length(growth) == doctest::Approx(1.0f));
	// 0.85 of a right angle leaves the axis 13.5 degrees above flat.
	CHECK(std::abs(growth.y) == doctest::Approx(std::sin(90.0f * kDeg * 0.15f)).epsilon(1e-3));
}

// The card is flat. Both the width axis and the arc must lie in the plane the growth axis
// defines, or the tapered quad folds and its face stops pointing at the camera.
TEST_CASE("the width axis and the arc stay in the blade's plane") {
	for (float deg : {0.0f, 30.0f, 60.0f, 85.0f}) {
		for (float lean_deg : {0.0f, 45.0f, 90.0f, 180.0f, 300.0f}) {
			const Frame f = frame_at(deg * kDeg, 0.7f, lean_deg * kDeg, 0.85f);
			CHECK(shader::dot(f.side, f.growth) == doctest::Approx(0.0f).epsilon(1e-3));
			CHECK(shader::dot(f.arc, f.growth) == doctest::Approx(0.0f).epsilon(1e-3));
			CHECK(shader::length(f.arc) == doctest::Approx(1.0f));
			CHECK(shader::length(f.growth) == doctest::Approx(1.0f));
			CHECK(shader::length(f.side) == doctest::Approx(1.0f));
		}
	}
}

// A lean that already runs along the growth axis projects to nothing, and the fallback has
// to be a real direction rather than a normalized zero vector.
TEST_CASE("an arc running along the growth axis falls back to the width axis") {
	const shader::vec3 growth(0.0f, 1.0f, 0.0f);
	const shader::vec3 side(1.0f, 0.0f, 0.0f);
	const shader::vec3 arc = shader::grass_plane_arc(growth, growth, side);
	CHECK(std::isfinite(arc.x));
	CHECK(std::isfinite(arc.y));
	CHECK(std::isfinite(arc.z));
	CHECK(shader::length(arc) == doctest::Approx(1.0f));
	CHECK(arc.x == doctest::Approx(1.0f));
}

// Direction independence: the tilt is a function of pitch alone. If it leaked azimuth in,
// orbiting the camera would visibly comb the field.
TEST_CASE("the projected area depends on pitch and not on azimuth") {
	for (float deg : {0.0f, 35.26f, 60.0f, 85.0f}) {
		const float elev = deg * kDeg;
		const Frame a = frame_at(elev, 0.0f, 1.1f, 0.85f);
		const Frame b = frame_at(elev, 2.0f, 1.1f, 0.85f);
		const Frame c = frame_at(elev, 5.1f, 1.1f, 0.85f);
		CHECK(projected_area(a, elev, 0.0f) ==
				doctest::Approx(projected_area(b, elev, 2.0f)).epsilon(1e-4));
		CHECK(projected_area(a, elev, 0.0f) ==
				doctest::Approx(projected_area(c, elev, 5.1f)).epsilon(1e-4));
	}
}
