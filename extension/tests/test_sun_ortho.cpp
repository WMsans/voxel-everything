#include <doctest/doctest.h>
#include "shade/sun_ortho.h"
#include "shade/cel.h"
#include <algorithm>
#include <cmath>

namespace {

// clip = M * (p, 1), then NDC = clip.xyz / clip.w. The ortho matrix has w = 1 everywhere,
// but dividing anyway is what the shader does, so the test does it too.
void project(const ve::SunOrtho &o, const float p[3], float ndc[3]) {
	float c[4];
	for (int r = 0; r < 4; r++)
		c[r] = o.view_proj[0 * 4 + r] * p[0] + o.view_proj[1 * 4 + r] * p[1] +
				o.view_proj[2 * 4 + r] * p[2] + o.view_proj[3 * 4 + r];
	for (int i = 0; i < 3; i++) ndc[i] = c[i] / c[3];
}

const float kLo[3] = {0.0f, -51.2f, 0.0f};
const float kHi[3] = {4096.0f, 972.8f, 4096.0f};

} // namespace

TEST_CASE("every corner of the world box lands inside the unit cube") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(o.valid);
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? kHi[0] : kLo[0], (i & 2) ? kHi[1] : kLo[1],
				(i & 4) ? kHi[2] : kLo[2]};
		float ndc[3];
		project(o, p, ndc);
		CHECK(ndc[0] >= -1.0001f);
		CHECK(ndc[0] <= 1.0001f);
		CHECK(ndc[1] >= -1.0001f);
		CHECK(ndc[1] <= 1.0001f);
		CHECK(ndc[2] >= -0.0001f);
		CHECK(ndc[2] <= 1.0001f);
	}
}

// Reverse-Z, matching every other depth surface in this engine (M1 errata 2): nearer to the
// light is LARGER. Get this backwards and the shadow test inverts -- everything lit is dark
// and everything dark is lit, which looks like an art choice until you move the camera.
TEST_CASE("moving toward the sun increases the depth") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const float base[3] = {2048.0f, 400.0f, 2048.0f};
	const float toward[3] = {base[0] + ve::kSunDir[0] * 100.0f,
			base[1] + ve::kSunDir[1] * 100.0f, base[2] + ve::kSunDir[2] * 100.0f};
	float a[3];
	float b[3];
	project(o, base, a);
	project(o, toward, b);
	CHECK(b[2] > a[2]);
}

TEST_CASE("moving along the sun direction moves only the depth") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const float base[3] = {2048.0f, 400.0f, 2048.0f};
	const float toward[3] = {base[0] + ve::kSunDir[0] * 250.0f,
			base[1] + ve::kSunDir[1] * 250.0f, base[2] + ve::kSunDir[2] * 250.0f};
	float a[3];
	float b[3];
	project(o, base, a);
	project(o, toward, b);
	CHECK(b[0] == doctest::Approx(a[0]).epsilon(1e-4));
	CHECK(b[1] == doctest::Approx(a[1]).epsilon(1e-4));
}

// A point displaced perpendicular to the sun must move in x or y, or the basis has
// collapsed and the whole world projects onto a line.
TEST_CASE("the light basis is non-degenerate") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const float base[3] = {2048.0f, 400.0f, 2048.0f};
	// Any vector not parallel to the sun; (0,1,0) is not, since kSunDir has x and z.
	const float side[3] = {base[0], base[1] + 200.0f, base[2]};
	float a[3];
	float b[3];
	project(o, base, a);
	project(o, side, b);
	CHECK((std::fabs(b[0] - a[0]) + std::fabs(b[1] - a[1])) > 1e-3f);
}

TEST_CASE("the texel size is the light-space extent over the map size") {
	const ve::SunOrtho a = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const ve::SunOrtho b = ve::sun_ortho(ve::kSunDir, kLo, kHi, 1024);
	CHECK(a.texel_world > 0.0f);
	CHECK(b.texel_world == doctest::Approx(a.texel_world * 2.0f).epsilon(1e-4));
	// A 4 km world in a 2048 map is about 2 m a texel (spec section 7 says "~2m/texel").
	// The light-space extent of a rotated box is larger than the box, so allow the range.
	CHECK(a.texel_world > 1.5f);
	CHECK(a.texel_world < 5.0f);
}

TEST_CASE("a degenerate request is refused rather than producing a silent identity") {
	const float zero[3] = {0, 0, 0};
	CHECK_FALSE(ve::sun_ortho(zero, kLo, kHi, 2048).valid);
	CHECK_FALSE(ve::sun_ortho(ve::kSunDir, kHi, kLo, 2048).valid); // inverted bounds
	CHECK_FALSE(ve::sun_ortho(ve::kSunDir, kLo, kHi, 0).valid);
}

TEST_CASE("the matrix depends on nothing but the sun and the bounds") {
	const ve::SunOrtho a = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const ve::SunOrtho b = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	for (int i = 0; i < 16; i++) CHECK(a.view_proj[i] == doctest::Approx(b.view_proj[i]));
}

TEST_CASE("depth_range is the world box's extent along the light axis") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(o.valid);
	// Light-space depth runs along -kSunDir (away from the sun). kSunDir is unit length,
	// so projecting the corners onto it gives metres directly.
	float mn = 1e30f;
	float mx = -1e30f;
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? kHi[0] : kLo[0], (i & 2) ? kHi[1] : kLo[1],
				(i & 4) ? kHi[2] : kLo[2]};
		const float c = -(p[0] * ve::kSunDir[0] + p[1] * ve::kSunDir[1] +
				p[2] * ve::kSunDir[2]);
		mn = std::min(mn, c);
		mx = std::max(mx, c);
	}
	CHECK(o.depth_range == doctest::Approx(mx - mn).epsilon(1e-4));
}

// The regression pin for the flat far field. deferred.comp.glsl compares a bias against
// NORMALIZED depth, so the texel size it scales must be normalized too. texel_world alone is
// metres: at this world's size it is ~2.7, which as a bias over a [0,1] depth range reports
// every pixel lit. The ratio is the quantity the shader actually needs.
TEST_CASE("one shadow texel is a small fraction of the depth range") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(o.valid);
	REQUIRE(o.depth_range > 0.0f);
	CHECK(o.texel_world / o.depth_range < 0.01f);
	// And the un-normalized value is nowhere near usable as a depth bias. This is the bug.
	CHECK(o.texel_world > 0.1f);
}

// Deriving the light basis from cross(l, worldUp) is well defined but badly CONDITIONED near
// the zenith: a small azimuth change swings the derived basis through a large rotation, so an
// animated sun crossing overhead makes the shadow map spin about the light axis. The demo's
// DirectionalLight3D sits at 84.99 degrees elevation, right in that band. A node supplies its
// own orthonormal basis, which rotates continuously and has no degenerate case.
TEST_CASE("the explicit-basis overload is continuous through the zenith") {
	const float eps = 1e-3f;
	// Two sun directions a hair either side of straight up, differing only in azimuth.
	const float a_dir[3] = {eps, 1.0f, 0.0f};
	const float b_dir[3] = {-eps, 1.0f, 0.0f};
	// A basis that barely moves between them, as a real animated node's would.
	const float right[3] = {0.0f, 0.0f, 1.0f};
	const float up_a[3] = {1.0f, 0.0f, 0.0f};
	const float up_b[3] = {1.0f, 0.0f, 0.0f};
	const ve::SunOrtho a = ve::sun_ortho(a_dir, right, up_a, kLo, kHi, 2048);
	const ve::SunOrtho b = ve::sun_ortho(b_dir, right, up_b, kLo, kHi, 2048);
	REQUIRE(a.valid);
	REQUIRE(b.valid);
	for (int i = 0; i < 16; i++) CHECK(b.view_proj[i] == doctest::Approx(a.view_proj[i]).epsilon(1e-2));

	// The derived-basis overload, given the same two directions, does NOT stay close: its
	// right vector flips through 180 degrees as the azimuth crosses over.
	const ve::SunOrtho da = ve::sun_ortho(a_dir, kLo, kHi, 2048);
	const ve::SunOrtho db = ve::sun_ortho(b_dir, kLo, kHi, 2048);
	REQUIRE(da.valid);
	REQUIRE(db.valid);
	float max_delta = 0.0f;
	for (int i = 0; i < 16; i++)
		max_delta = std::max(max_delta, std::fabs(db.view_proj[i] - da.view_proj[i]));
	CHECK(max_delta > 1e-4f);
}

TEST_CASE("an explicit basis reproduces the derived one when they agree") {
	const ve::SunOrtho derived = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(derived.valid);
	// Rebuild kSunDir's derived basis by hand and hand it back in: same matrix.
	const float l[3] = {-ve::kSunDir[0], -ve::kSunDir[1], -ve::kSunDir[2]};
	float r[3] = {l[1] * 0.0f - l[2] * 1.0f, l[2] * 0.0f - l[0] * 0.0f, l[0] * 1.0f - l[1] * 0.0f};
	const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	for (int i = 0; i < 3; i++) r[i] /= rl;
	float u[3] = {r[1] * l[2] - r[2] * l[1], r[2] * l[0] - r[0] * l[2], r[0] * l[1] - r[1] * l[0]};
	const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	for (int i = 0; i < 3; i++) u[i] /= ul;
	const ve::SunOrtho explicit_basis = ve::sun_ortho(ve::kSunDir, r, u, kLo, kHi, 2048);
	REQUIRE(explicit_basis.valid);
	for (int i = 0; i < 16; i++)
		CHECK(explicit_basis.view_proj[i] == doctest::Approx(derived.view_proj[i]).epsilon(1e-5));
	CHECK(explicit_basis.depth_range == doctest::Approx(derived.depth_range).epsilon(1e-5));
}
