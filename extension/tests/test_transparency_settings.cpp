#include <doctest/doctest.h>
#include "transparency/transparency_settings.h"
#include "transparency/transparency_settings_store.h"
#include <cmath>

TEST_CASE("transparency defaults are the spec's and sit inside their own clamp") {
	ve::TransparencySettings s;
	CHECK(s.enabled);
	CHECK(s.min_transmit == doctest::Approx(0.35f));
	CHECK(s.sky_thickness_m == doctest::Approx(4.0f));
	ve::TransparencySettings c = s;
	ve::clamp_transparency_settings(&c);
	CHECK(c.min_transmit == doctest::Approx(s.min_transmit));
	CHECK(c.sky_thickness_m == doctest::Approx(s.sky_thickness_m));
}

// A floor of 1 would make every transparent material invisible glass; a NaN must not
// reach the shader.
TEST_CASE("transparency clamp keeps the floor a floor") {
	ve::TransparencySettings s;
	s.min_transmit = 0.0f / 0.0f;
	s.sky_thickness_m = -3.0f;
	ve::clamp_transparency_settings(&s);
	CHECK(s.min_transmit >= 0.0f);
	CHECK(s.min_transmit <= 0.95f);
	CHECK(s.sky_thickness_m >= 0.0f);
	s.min_transmit = 7.0f;
	ve::clamp_transparency_settings(&s);
	CHECK(s.min_transmit <= 0.95f);
}

TEST_CASE("the transparency store round-trips every knob and clamps on the way in") {
	ve::TransparencySettingsStore store;
	CHECK(store.set_value("enabled", 0.0f));
	CHECK(store.get().enabled == false);
	CHECK(store.set_value("min_transmit", 0.5f));
	CHECK(store.value("min_transmit") == doctest::Approx(0.5f));
	CHECK(store.set_value("sky_thickness_m", 1e9f));
	CHECK(store.value("sky_thickness_m") <= 50.0f);
	CHECK_FALSE(store.set_value("min_step_m", 1.0f)); // the walker's knobs are gone
	CHECK_FALSE(store.set_value("max_steps", 1.0f));
}