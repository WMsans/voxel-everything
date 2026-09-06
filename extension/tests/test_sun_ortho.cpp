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

// ---------------------------------------------------------------------------------------
// The camera-following ortho. An unbounded world has no box to fit, so the map covers a
// neighbourhood of the camera -- and the moment the fitted set moves with the camera, the
// two properties below stop being free and have to be engineered.
// ---------------------------------------------------------------------------------------

namespace {

const float kCenter[3] = {512.0f, 60.0f, 512.0f};
constexpr float kRadius = 1638.4f; // WorldConfig::stream_radius_m, the shipped default
constexpr int kMap = 2048;

// Where a world point lands in the shadow map, in texels.
void texel_of(const ve::SunOrtho &o, const float p[3], float uv[2]) {
	float ndc[3];
	project(o, p, ndc);
	uv[0] = (ndc[0] * 0.5f + 0.5f) * float(kMap);
	uv[1] = (ndc[1] * 0.5f + 0.5f) * float(kMap);
}

ve::SunOrtho at(float dx, float dz) {
	const float c[3] = {kCenter[0] + dx, kCenter[1], kCenter[2] + dz};
	return ve::sun_ortho_sphere(ve::kSunDir, c, kRadius, kMap);
}

} // namespace

// THE SHIMMER PIN. A shadow map whose texel grid slides with the camera re-quantises every
// silhouette in it every frame: edge texels flip coverage, and the shadow crawls. The grid
// must be pinned in WORLD space, so camera motion may translate the map by whole texels and
// by nothing else -- which is to say a static point keeps its position WITHIN its texel.
TEST_CASE("a static point keeps its sub-texel position as the camera moves") {
	const float feature[3] = {480.0f, 52.0f, 540.0f};
	float base[2];
	texel_of(at(0.0f, 0.0f), feature, base);
	// Sub-texel steps, then several texels' worth: both must land on the same grid.
	for (float d = 0.05f; d < 40.0f; d *= 1.7f) {
		float moved[2];
		texel_of(at(d, -0.6f * d), feature, moved);
		for (int a = 0; a < 2; a++) {
			const float shift = moved[a] - base[a];
			CHECK(std::fabs(shift - std::round(shift)) < 2e-3f);
		}
	}
}

// The snap above is only meaningful if the grid it snaps to has a FIXED spacing. Fitting an
// axis-aligned cube makes the light-space extent -- and so the texel size -- a function of
// the sun's direction; fitting the sphere makes it a constant, because a sphere's silhouette
// is the same circle from every angle.
TEST_CASE("the texel size does not depend on where the sun is") {
	const float dirs[4][3] = {{0.5746958f, 0.7662610f, 0.2873479f}, {0.0f, 1.0f, 0.0f},
			{0.7071f, 0.7071f, 0.0f}, {-0.3f, 0.5f, 0.81f}};
	const ve::SunOrtho first = ve::sun_ortho_sphere(dirs[0], kCenter, kRadius, kMap);
	REQUIRE(first.valid);
	for (int i = 1; i < 4; i++) {
		const ve::SunOrtho o = ve::sun_ortho_sphere(dirs[i], kCenter, kRadius, kMap);
		REQUIRE(o.valid);
		CHECK(o.texel_world == doctest::Approx(first.texel_world).epsilon(1e-5));
		CHECK(o.depth_range == doctest::Approx(first.depth_range).epsilon(1e-5));
	}
}

// SunShadowPass::build() rebuilds the map whenever the matrix differs, so an unsnapped
// camera-following matrix means a full 2048^2 depth pass over the whole cut EVERY FRAME the
// camera moves. Inside one texel of travel the matrix must be bit-identical.
TEST_CASE("the matrix is unchanged while the camera moves less than a texel") {
	const ve::SunOrtho a = at(0.0f, 0.0f);
	REQUIRE(a.valid);
	const float step = a.texel_world * 0.05f;
	const ve::SunOrtho b = at(step, step);
	REQUIRE(b.valid);
	for (int i = 0; i < 16; i++) CHECK(b.view_proj[i] == a.view_proj[i]);
}

// The unbounded world's whole point is that there is no origin to stay near, and the snap is
// a quantisation of coordinates that grow without bound. Do it in float and the quotient
// rounds coarsely enough to reintroduce the very drift it removes; the matrix itself stays
// float32 (the GPU wants it that way), which is fine because what must be exact is the
// DIFFERENCE between two frames' matrices, not their absolute values.
TEST_CASE("the grid stays pinned a thousand kilometres from the origin") {
	for (int e = 0; e <= 6; e += 2) {
		const float base = std::pow(10.0f, float(e));
		const float feature[3] = {base, 52.0f, base + 30.0f};
		float first[2] = {0.0f, 0.0f};
		for (int i = 0; i <= 40; i++) {
			const float c[3] = {base + 0.05f * float(i), 60.0f, base - 0.03f * float(i)};
			const ve::SunOrtho o = ve::sun_ortho_sphere(ve::kSunDir, c, kRadius, kMap);
			REQUIRE(o.valid);
			float ndc[3];
			project(o, feature, ndc);
			const float uv[2] = {(ndc[0] * 0.5f + 0.5f) * float(kMap),
					(ndc[1] * 0.5f + 0.5f) * float(kMap)};
			if (i == 0) {
				first[0] = uv[0];
				first[1] = uv[1];
				continue;
			}
			for (int a = 0; a < 2; a++) {
				const float shift = uv[a] - first[a];
				CHECK(std::fabs(shift - std::round(shift)) < 2e-3f);
			}
		}
	}
}

// Snapping the min corner moves it DOWN by up to a texel, so the far side of the sphere must
// still be inside the map or the fix trades shimmer for a clipped horizon.
TEST_CASE("the fitted sphere is covered from every direction") {
	for (int i = 0; i < 8; i++) {
		const float dx = float((i % 4) - 2) * 0.37f * kRadius;
		const ve::SunOrtho o = at(dx, dx * 0.5f);
		REQUIRE(o.valid);
		// 26 points on the sphere: the axes, the face diagonals and the corners.
		for (int k = 0; k < 27; k++) {
			if (k == 13) continue;
			float v[3] = {float(k % 3 - 1), float((k / 3) % 3 - 1), float(k / 9 - 1)};
			const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			const float p[3] = {kCenter[0] + dx + v[0] / len * kRadius,
					kCenter[1] + v[1] / len * kRadius,
					kCenter[2] + dx * 0.5f + v[2] / len * kRadius};
			float ndc[3];
			project(o, p, ndc);
			CHECK(ndc[0] >= -1.0f);
			CHECK(ndc[0] <= 1.0f);
			CHECK(ndc[1] >= -1.0f);
			CHECK(ndc[1] <= 1.0f);
			CHECK(ndc[2] >= 0.0f);
			CHECK(ndc[2] <= 1.0f);
		}
	}
}

// The sphere is the set the shadow cut actually draws (LodTree::shadow_visit keeps chunks
// within stream_radius_m of the camera, a distance test). Fitting the enclosing CUBE instead
// spends the map on eight empty corners, and the wasted extent lands on every texel.
TEST_CASE("the sphere fit is finer than the cube it replaces") {
	const ve::SunOrtho sphere = ve::sun_ortho_sphere(ve::kSunDir, kCenter, kRadius, kMap);
	const float lo[3] = {kCenter[0] - kRadius, kCenter[1] - kRadius, kCenter[2] - kRadius};
	const float hi[3] = {kCenter[0] + kRadius, kCenter[1] + kRadius, kCenter[2] + kRadius};
	const ve::SunOrtho cube = ve::sun_ortho(ve::kSunDir, lo, hi, kMap);
	REQUIRE(sphere.valid);
	REQUIRE(cube.valid);
	CHECK(sphere.texel_world < cube.texel_world * 0.7f);
	// And the depth range is no shallower than the cube's on either side, so nothing that
	// used to cast stops casting.
	CHECK(sphere.depth_range >= cube.depth_range);
}

TEST_CASE("a degenerate sphere request is refused") {
	CHECK_FALSE(ve::sun_ortho_sphere(ve::kSunDir, kCenter, 0.0f, kMap).valid);
	CHECK_FALSE(ve::sun_ortho_sphere(ve::kSunDir, kCenter, kRadius, 1).valid);
	const float zero[3] = {0, 0, 0};
	CHECK_FALSE(ve::sun_ortho_sphere(zero, kCenter, kRadius, kMap).valid);
}

// The explicit-basis overload exists for an animated light (see above); the sphere fit must
// offer it too, or a moving sun would have to give up the stable grid to keep continuity.
TEST_CASE("the sphere fit accepts the light's own basis") {
	const float l[3] = {-ve::kSunDir[0], -ve::kSunDir[1], -ve::kSunDir[2]};
	float r[3] = {-l[2], 0.0f, l[0]};
	const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	for (int i = 0; i < 3; i++) r[i] /= rl;
	const float u[3] = {r[1] * l[2] - r[2] * l[1], r[2] * l[0] - r[0] * l[2],
			r[0] * l[1] - r[1] * l[0]};
	const ve::SunOrtho derived = ve::sun_ortho_sphere(ve::kSunDir, kCenter, kRadius, kMap);
	const ve::SunOrtho given = ve::sun_ortho_sphere(ve::kSunDir, r, u, kCenter, kRadius, kMap);
	REQUIRE(derived.valid);
	REQUIRE(given.valid);
	for (int i = 0; i < 16; i++)
		CHECK(given.view_proj[i] == doctest::Approx(derived.view_proj[i]).epsilon(1e-5));
}

// CHARACTERIZATION (Task 0). The exact fit that ships today at the old default radius.
// Task 1 introduces cascades; the outermost cascade must reproduce THIS matrix, bit for
// bit, at this radius. If cascades change these numbers, they changed what ships.
TEST_CASE("characterization: the shipping fit at stream_radius 1638.4") {
	const float cam[3] = {800.0f, 60.0f, 800.0f};
	const ve::SunOrtho o = ve::sun_ortho_sphere(ve::kSunDir, cam, 1638.4f, 2048);
	REQUIRE(o.valid);
	// 2 * 1638.4 / 2047 -- the texel depends on the radius and the map size and on
	// nothing else: not the sun's direction, not the camera's position.
	CHECK(o.texel_world == doctest::Approx(1.600782f).epsilon(1e-5));
	// 4R: the sphere spans 2R with R of margin on each side (fit_sphere's half_depth).
	CHECK(o.depth_range == doctest::Approx(4.0f * 1638.4f).epsilon(1e-4));
}
