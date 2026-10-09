#include <doctest/doctest.h>
#include "conifers/conifer_layout.h"
#include "conifers/conifer_settings_store.h"
#include <cmath>
#include <cstring>

namespace {
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
const float kOrigin[3] = {0.0f, 0.0f, 0.0f};
} // namespace

TEST_CASE("conifer settings clamp every knob, NaN included, and idempotently") {
	ve::ConiferSettings s;
	s.card_reach_m = NAN;
	s.impostor_reach_m = 1.0e9f;
	s.clumps_per_tree = 4096;
	s.tier_roundness = -3.0f;
	ve::clamp_conifer_settings(&s);
	CHECK(std::isfinite(s.card_reach_m));
	CHECK(s.impostor_reach_m <= 4000.0f);
	CHECK(s.clumps_per_tree == 128);
	CHECK(s.tier_roundness == 0.0f);
	ve::ConiferSettings t = s;
	ve::clamp_conifer_settings(&t);
	CHECK(std::memcmp(&s, &t, sizeof(s)) == 0);
}

TEST_CASE("the conifer store round-trips a knob by name and clamps on write") {
	ve::ConiferSettingsStore store;
	CHECK(store.set_value("card_reach_m", 220.0f));
	CHECK(store.value("card_reach_m") == doctest::Approx(220.0f));
	store.set_value("card_reach_m", 1.0e9f);
	CHECK(store.value("card_reach_m") <= 1000.0f);
}

TEST_CASE("the conifer pass block is 208 bytes with cam first") {
	CHECK(sizeof(ve::ConiferPassParams) == 208);
	ve::ConiferSettings s;
	const float cam[3] = {12.0f, 34.0f, -56.0f};
	const ve::ConiferLayout l = ve::conifer_layout(s, 8.0f, cam, kIdentity, 0.0f, 0.0f);
	CHECK(l.params.cam[0] == 12.0f);
	CHECK(l.params.cam[2] == -56.0f);
	CHECK(l.params.cam[3] == s.card_reach_m);
	CHECK(l.params.reach[0] == s.impostor_reach_m);
}

TEST_CASE("the cell box covers the imposter reach in shifted space") {
	ve::ConiferSettings s;
	const float cam[3] = {100.0f, 0.0f, -40.0f};
	const ve::ConiferLayout l = ve::conifer_layout(s, 8.0f, cam, kIdentity, 1000.0f, -2000.0f);
	const float sx = 1100.0f, sz = -2040.0f;
	CHECK(float(l.params.cell_min[0]) * 8.0f <= sx - s.impostor_reach_m);
	CHECK(float(l.params.cell_min[0] + l.params.cell_dim[0]) * 8.0f >= sx + s.impostor_reach_m);
	CHECK(float(l.params.cell_min[2]) * 8.0f <= sz - s.impostor_reach_m);
	CHECK(float(l.params.cell_min[2] + l.params.cell_dim[2]) * 8.0f >= sz + s.impostor_reach_m);
	CHECK(l.dispatch_threads == l.params.cell_dim[0] * l.params.cell_dim[2]);
	CHECK(l.params.cell_min[3] == l.dispatch_threads);
}

TEST_CASE("a disabled layout dispatches nothing") {
	ve::ConiferSettings s;
	s.enabled = false;
	CHECK(ve::conifer_layout(s, 8.0f, kOrigin, kIdentity, 0.0f, 0.0f).dispatch_threads == 0);
}

TEST_CASE("the clump budget falls monotonically to an eighth at the card reach") {
	int prev = 1 << 30;
	for (int d = 0; d <= 300; d += 5) {
		const int b = ve::conifer_clump_budget(128, 300.0f, float(d));
		CHECK(b <= prev);
		CHECK(b >= 1);
		prev = b;
	}
	CHECK(ve::conifer_clump_budget(128, 300.0f, 0.0f) == 128);
	CHECK(ve::conifer_clump_budget(128, 300.0f, 300.0f) == 16);
}

TEST_CASE("the card raster block carries the conifer palette, reach, wind and style") {
	ve::ConiferSettings s;
	const ve::ConiferLayout l = ve::conifer_layout(s, 8.0f, kOrigin, kIdentity, 0.0f, 0.0f);
	CHECK(l.raster.cam[3] == s.card_reach_m); // leaf.frag's fade reads cam.w
	CHECK(l.raster.palette_top[1] == s.palette_top_g);
	CHECK(l.raster.palette_under[2] == s.palette_under_b);
	CHECK(l.raster.wind[0] == s.wind_strength);
	CHECK(l.raster.style[2] == s.leaf_grain);
}
