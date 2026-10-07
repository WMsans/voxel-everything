#include <doctest/doctest.h>
#include "water/water_settings.h"
#include "water/water_settings_store.h"

TEST_CASE("water defaults are the spec's and sit inside their own clamp") {
	ve::WaterSettings s;
	CHECK(s.wave_strength == doctest::Approx(1.0f));
	CHECK(s.flow_speed == doctest::Approx(0.6f));
	CHECK(s.refraction_strength == doctest::Approx(0.03f));
	CHECK(s.foam_width_m == doctest::Approx(0.3f));
	ve::WaterSettings c = s;
	ve::clamp_water_settings(&c);
	CHECK(c.wave_strength == doctest::Approx(s.wave_strength));
	CHECK(c.flow_speed == doctest::Approx(s.flow_speed));
	CHECK(c.refraction_strength == doctest::Approx(s.refraction_strength));
	CHECK(c.foam_width_m == doctest::Approx(s.foam_width_m));
}

// A NaN or a negative must not reach the shader: a negative flow would run water uphill and a
// huge refraction offset samples pixels from the far side of the screen.
TEST_CASE("water clamp keeps every knob in range") {
	ve::WaterSettings s;
	s.wave_strength = 0.0f / 0.0f;
	s.flow_speed = -2.0f;
	s.refraction_strength = 9.0f;
	s.foam_width_m = -1.0f;
	ve::clamp_water_settings(&s);
	CHECK(s.wave_strength >= 0.0f);
	CHECK(s.wave_strength <= 3.0f);
	CHECK(s.flow_speed == doctest::Approx(0.0f));
	CHECK(s.refraction_strength <= 0.2f);
	CHECK(s.foam_width_m == doctest::Approx(0.0f));
}

TEST_CASE("the water store round-trips every knob and clamps on the way in") {
	ve::WaterSettingsStore store;
	CHECK(store.set_value("wave_strength", 2.0f));
	CHECK(store.value("wave_strength") == doctest::Approx(2.0f));
	CHECK(store.set_value("flow_speed", 1.5f));
	CHECK(store.get().flow_speed == doctest::Approx(1.5f));
	CHECK(store.set_value("refraction_strength", 1e9f));
	CHECK(store.value("refraction_strength") <= 0.2f);
	CHECK(store.set_value("foam_width_m", 0.0f));
	CHECK(store.get().foam_width_m == doctest::Approx(0.0f));
	CHECK_FALSE(store.set_value("min_transmit", 0.5f)); // transparency's knob, not water's
}
