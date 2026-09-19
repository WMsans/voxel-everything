#include <doctest/doctest.h>
#include "leaves/leaf_settings.h"
#include "leaves/leaf_settings_store.h"
#include <cmath>

TEST_CASE("leaf settings clamp every knob, NaN included") {
	ve::LeafSettings s;
	s.reach_m = 1.0e9f;
	s.clumps_per_tree = 100000;
	s.clump_radius_m = -4.0f;
	s.canopy_roundness = std::nanf("");
	s.wind_strength = -1.0f;
	ve::clamp_leaf_settings(&s);
	CHECK(s.reach_m <= 400.0f);
	CHECK(s.clumps_per_tree <= 128);
	CHECK(s.clump_radius_m >= 0.0f);
	CHECK(s.canopy_roundness >= 0.0f);
	CHECK(s.canopy_roundness <= 1.0f);
	CHECK(s.wind_strength >= 0.0f);
}

TEST_CASE("clamping is idempotent") {
	ve::LeafSettings a;
	ve::clamp_leaf_settings(&a);
	ve::LeafSettings b = a;
	ve::clamp_leaf_settings(&b);
	CHECK(a.reach_m == b.reach_m);
	CHECK(a.clumps_per_tree == b.clumps_per_tree);
	CHECK(a.canopy_roundness == b.canopy_roundness);
}

TEST_CASE("clumps_per_tree is capped at the scatter workgroup width") {
	// shaders/leaf_scatter.comp.glsl runs one workgroup per tree with local_size_x = 128,
	// one thread per candidate clump, so a tree must never need a second group.
	ve::LeafSettings s;
	s.clumps_per_tree = 1000;
	ve::clamp_leaf_settings(&s);
	CHECK(s.clumps_per_tree <= 128);
}

TEST_CASE("the store round-trips a knob by name and clamps on write") {
	ve::LeafSettingsStore store;
	CHECK(store.set_value("reach_m", 180.0f));
	CHECK(store.get().reach_m == doctest::Approx(180.0f));
	CHECK(store.set_value("reach_m", 1.0e9f));
	CHECK(store.get().reach_m <= 400.0f);
	CHECK_FALSE(store.set_value("no_such_knob", 1.0f));
}

#include "leaves/leaf_layout.h"

namespace {
// A finite, well-conditioned view-projection: identity is enough for the layout arithmetic,
// which only needs the frustum planes to be extractable, not meaningful.
const float kIdentity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
const float kOrigin[3] = {0.0f, 0.0f, 0.0f};
} // namespace

TEST_CASE("the params block is exactly 256 bytes") {
	// std140 padding cannot disagree with the C++ struct if the struct is 16 vec4.
	CHECK(sizeof(ve::LeafParams) == 256);
}

TEST_CASE("cam position is the first three floats of the params block") {
	ve::LeafSettings s;
	const float cam[3] = {12.0f, 34.0f, -56.0f};
	const ve::LeafLayout l = ve::leaf_layout(s, cam, kIdentity);
	CHECK(l.params.cam[0] == doctest::Approx(12.0f));
	CHECK(l.params.cam[1] == doctest::Approx(34.0f));
	CHECK(l.params.cam[2] == doctest::Approx(-56.0f));
}

TEST_CASE("the clump budget falls monotonically with distance") {
	ve::LeafSettings s;
	s.reach_m = 250.0f;
	s.clumps_per_tree = 96;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity);
	int prev = ve::leaf_clump_budget(l, 0.0f);
	CHECK(prev == 96);
	for (float d = 5.0f; d <= 250.0f; d += 5.0f) {
		const int n = ve::leaf_clump_budget(l, d);
		CHECK(n <= prev);
		CHECK(n >= 1); // a tree inside the reach always keeps SOME canopy
		prev = n;
	}
	CHECK(ve::leaf_clump_budget(l, 260.0f) == 0); // past the reach, nothing
}

TEST_CASE("fewer clumps are exactly compensated by larger ones") {
	// The whole point of the density LOD: total card area per tree is flat in distance, so a
	// distant crown keeps its silhouette instead of thinning into scattered dots.
	ve::LeafSettings s;
	s.reach_m = 250.0f;
	s.clumps_per_tree = 96;
	s.clump_radius_m = 0.85f;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity);
	const float near_area = 96.0f * 0.85f * 0.85f;
	for (float d = 0.0f; d < 250.0f; d += 10.0f) {
		const float r = ve::leaf_clump_radius(l, d);
		const float area = float(ve::leaf_clump_budget(l, d)) * r * r;
		CHECK(area == doctest::Approx(near_area).epsilon(0.12f));
	}
}

TEST_CASE("the cell box covers the reach and capacity bounds the dispatch") {
	ve::LeafSettings s;
	s.reach_m = 250.0f;
	const float cam[3] = {1000.0f, 60.0f, -2000.0f};
	const ve::LeafLayout l = ve::leaf_layout(s, cam, kIdentity);
	// Every cell whose centre is within the reach must be inside the box.
	const float cell = l.cell_size_m;
	CHECK(float(l.cell_min.x) * cell <= cam[0] - s.reach_m);
	CHECK(float(l.cell_min.z) * cell <= cam[2] - s.reach_m);
	CHECK(float(l.cell_min.x + l.cell_dim.x) * cell >= cam[0] + s.reach_m);
	CHECK(float(l.cell_min.z + l.cell_dim.z) * cell >= cam[2] + s.reach_m);
	CHECK(l.dispatch_threads == l.cell_dim.x * l.cell_dim.z);
	CHECK(l.max_trees <= s.max_trees);
	CHECK(l.max_clumps <= s.max_clumps);
}

TEST_CASE("a disabled or zero-reach layout dispatches nothing") {
	ve::LeafSettings s;
	s.enabled = false;
	CHECK(ve::leaf_layout(s, kOrigin, kIdentity).dispatch_threads == 0);
	s.enabled = true;
	s.reach_m = 0.0f;
	CHECK(ve::leaf_layout(s, kOrigin, kIdentity).dispatch_threads == 0);
}

TEST_CASE("an unclamped snapshot is clamped internally") {
	ve::LeafSettings s;
	s.reach_m = 1.0e9f;
	s.clumps_per_tree = 100000;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity);
	CHECK(l.reach_m <= 400.0f);
	CHECK(ve::leaf_clump_budget(l, 0.0f) <= 128);
}
