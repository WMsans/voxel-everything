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
