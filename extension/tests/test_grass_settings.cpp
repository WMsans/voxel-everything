#include <doctest/doctest.h>
#include "grass/grass_settings.h"
#include "grass/grass_settings_store.h"

TEST_CASE("grass defaults are inside their own clamp") {
	ve::GrassSettings s;
	ve::GrassSettings c = s;
	ve::clamp_grass_settings(&c);
	CHECK(c.enabled == s.enabled);
	CHECK(c.reach_m == doctest::Approx(s.reach_m));
	CHECK(c.blades_per_brick == s.blades_per_brick);
	CHECK(c.max_blades == s.max_blades);
	CHECK(c.blade_width_m == doctest::Approx(s.blade_width_m));
	CHECK(c.blade_height_m == doctest::Approx(s.blade_height_m));
	CHECK(c.wind_strength == doctest::Approx(s.wind_strength));
}

TEST_CASE("grass clamp pulls every knob back into range") {
	ve::GrassSettings s;
	s.reach_m = 1e9f;
	s.blades_per_brick = 4096;
	s.max_blades = -7;
	s.blade_width_m = -1.0f;
	s.blade_height_m = 1e9f;
	s.wind_strength = -3.0f;
	s.slope_cos_min = 7.0f;
	ve::clamp_grass_settings(&s);
	CHECK(s.reach_m <= 256.0f);
	CHECK(s.blades_per_brick <= 64);
	CHECK(s.max_blades >= 0);
	CHECK(s.blade_width_m >= 0.0f);
	CHECK(s.blade_height_m <= 4.0f);
	CHECK(s.wind_strength >= 0.0f);
	CHECK(s.slope_cos_min <= 1.0f);
}

// The reach is what sizes stage 1's dispatch, and that grows with its cube. A clamp that
// let it through unbounded would be a hang, not a visual bug.
TEST_CASE("grass reach is hard-bounded even from a NaN") {
	ve::GrassSettings s;
	s.reach_m = 0.0f / 0.0f;
	ve::clamp_grass_settings(&s);
	CHECK(s.reach_m >= 0.0f);
	CHECK(s.reach_m <= 256.0f);
}

TEST_CASE("the store round-trips every named field and clamps on the way in") {
	ve::GrassSettingsStore store;
	CHECK(store.set_value("reach_m", 25.0f));
	CHECK(store.value("reach_m") == doctest::Approx(25.0f));
	CHECK(store.set_value("reach_m", 1e9f));
	CHECK(store.value("reach_m") <= 256.0f);
	CHECK(store.set_value("blades_per_brick", 8.0f));
	CHECK(store.value("blades_per_brick") == doctest::Approx(8.0f));
	CHECK(store.set_value("enabled", 0.0f));
	CHECK(store.value("enabled") == doctest::Approx(0.0f));
	CHECK(store.get().enabled == false);
	CHECK_FALSE(store.set_value("no_such_knob", 1.0f));
}
