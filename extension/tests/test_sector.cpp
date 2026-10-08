#include <doctest/doctest.h>
#include "terrain/sector.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace {
uint32_t pack(float r, float g) {
	const auto q = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
	return q(r) | (q(g) << 16);
}
// Fills a sector from f(x, z) evaluated at every texel centre, apron included -- what the
// bake does.
template <class F>
ve::SectorTexels fill(ve::SectorCoord c, F f) {
	ve::SectorTexels t;
	t.texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
	for (int z = 0; z < ve::kSectorStride; z++)
		for (int x = 0; x < ve::kSectorStride; x++) {
			float wx, wz;
			ve::sector_texel_pos(c, x, z, &wx, &wz);
			t.texels[size_t(z * ve::kSectorStride + x)] = pack(f(wx, wz), 0.5f);
		}
	return t;
}
} // namespace

TEST_CASE("sector_of floors negative coordinates") {
	CHECK(ve::sector_of(-0.1f, 409.6f) == ve::SectorCoord{-1, 1});
	CHECK(ve::sector_of(0.0f, -409.7f) == ve::SectorCoord{0, -2});
	CHECK(ve::sector_of(409.5f, 0.0f) == ve::SectorCoord{0, 0});
}

TEST_CASE("the first interior texel sits half a texel inside the sector corner") {
	float x, z;
	ve::sector_texel_pos({1, -1}, ve::kSectorApron, ve::kSectorApron, &x, &z);
	CHECK(x == doctest::Approx(409.6f + 0.8f));
	CHECK(z == doctest::Approx(-409.6f + 0.8f));
	ve::sector_texel_pos({0, 0}, 0, 0, &x, &z);
	CHECK(x == doctest::Approx(-2.4f)); // the apron reaches past the corner
}

TEST_CASE("sector_distance is zero inside and the gap outside") {
	CHECK(ve::sector_distance({0, 0}, 10.0f, 10.0f) == doctest::Approx(0.0f));
	CHECK(ve::sector_distance({0, 0}, -3.0f, 409.6f + 4.0f) == doctest::Approx(5.0f));
}

TEST_CASE("the B-spline reproduces a linear ramp and its slope") {
	const ve::SectorCoord c{-2, 3};
	const auto ramp = [](float x, float z) { return 0.4f + 0.0002f * (x + 900.0f) - 0.0001f * (z - 1300.0f); };
	const ve::SectorTexels t = fill(c, ramp);
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> u(0.0f, ve::kSectorSizeM);
	for (int i = 0; i < 500; i++) {
		const float x = c.x * ve::kSectorSizeM + u(rng), z = c.z * ve::kSectorSizeM + u(rng);
		const ve::SectorSample s = ve::sector_bspline(t, c, x, z);
		CHECK(s.r == doctest::Approx(ramp(x, z)).epsilon(1e-4));
		CHECK(s.drdx == doctest::Approx(0.0002f).epsilon(2e-2));
		CHECK(s.drdz == doctest::Approx(-0.0001f).epsilon(2e-2));
		CHECK(s.g == doctest::Approx(0.5f).epsilon(1e-4));
	}
}

// The spec's §4.5 bound: the B-spline's gradient is a convex combination of per-axis texel
// differences, so no sample can be steeper than the steepest texel step. This is what makes
// the field stage's declared bound a property of the texels the bake can check exactly.
TEST_CASE("no B-spline gradient exceeds the steepest per-axis texel step") {
	std::mt19937 rng(11);
	std::uniform_int_distribution<uint32_t> word(0, 65535);
	std::uniform_real_distribution<float> u(0.0f, ve::kSectorSizeM);
	for (int grid = 0; grid < 10; grid++) {
		ve::SectorTexels t;
		t.texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
		for (uint32_t &v : t.texels) v = word(rng) | (word(rng) << 16);
		const float limit = ve::sector_max_axis_slope(t) / ve::kSectorHeightSpanM; // r units per metre
		for (int i = 0; i < 2000; i++) {
			const ve::SectorSample s = ve::sector_bspline(t, {0, 0}, u(rng), u(rng));
			CHECK(std::abs(s.drdx) <= limit * 1.0001f);
			CHECK(std::abs(s.drdz) <= limit * 1.0001f);
		}
	}
}

TEST_CASE("two neighbouring sectors agree at their shared edge") {
	const auto f = [](float x, float z) { return 0.5f + 0.2f * std::sin(x * 0.03f) * std::cos(z * 0.02f); };
	const ve::SectorCoord a{0, 0}, b{1, 0};
	const ve::SectorTexels ta = fill(a, f), tb = fill(b, f);
	for (float z = 3.0f; z < ve::kSectorSizeM; z += 37.0f) {
		const float edge = ve::kSectorSizeM;
		const ve::SectorSample sa = ve::sector_bspline(ta, a, edge - 1e-3f, z);
		const ve::SectorSample sb = ve::sector_bspline(tb, b, edge + 1e-3f, z);
		CHECK(sa.r == doctest::Approx(sb.r).epsilon(1e-4));
		CHECK(sa.drdx == doctest::Approx(sb.drdx).epsilon(1e-2));
	}
}

TEST_CASE("max_axis_slope reports a known ramp in metres per metre") {
	ve::SectorTexels t;
	t.texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
	for (int z = 0; z < ve::kSectorStride; z++)
		for (int x = 0; x < ve::kSectorStride; x++)
			t.texels[size_t(z * ve::kSectorStride + x)] = uint32_t(100 * x + 30 * z);
	CHECK(ve::sector_max_axis_slope(t) ==
			doctest::Approx(100.0f / 65535.0f * ve::kSectorHeightSpanM / ve::kSectorTexelM));
}

TEST_CASE("the window wraps negatives and never aliases a sector out of reach") {
	CHECK(ve::sector_window_cell({-1, -1}) == ve::sector_window_cell({23, 23}));
	const std::vector<int32_t> w = ve::sector_window({0, 0}, {{{-11, 5}, 3}, {{13, 5}, 7}});
	CHECK(w.size() == size_t(ve::kSectorWindow * ve::kSectorWindow * 3));
	CHECK(ve::sector_window_lookup(w, {-11, 5}) == 3);
	CHECK(ve::sector_window_lookup(w, {13, 5}) == -1); // 13 > 11 from the centre: not written
	CHECK(ve::sector_window_lookup(w, {0, 0}) == -1);  // nothing resident there
}
