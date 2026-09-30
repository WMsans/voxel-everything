#include <doctest/doctest.h>
#include "transparency/transparency_settings.h"
#include "transparency/transparency_settings_store.h"
#include <cmath>

TEST_CASE("transparency defaults are the spec's and sit inside their own clamp") {
	ve::TransparencySettings s;
	CHECK(s.enabled);
	CHECK(s.min_step_m == doctest::Approx(0.05f));
	CHECK(s.max_steps == 48);
	CHECK(s.min_transmit == doctest::Approx(0.01f));
	ve::TransparencySettings c = s;
	ve::clamp_transparency_settings(&c);
	CHECK(c.min_step_m == doctest::Approx(s.min_step_m));
	CHECK(c.max_steps == s.max_steps);
	CHECK(c.min_transmit == doctest::Approx(s.min_transmit));
}

// The walker divides nothing by these, but a zero step never advances and a zero cap never
// walks: both would turn the medium into a wall. A NaN must not reach the shader either.
TEST_CASE("transparency clamp keeps the walk able to move") {
	ve::TransparencySettings s;
	s.min_step_m = 0.0f / 0.0f;
	s.max_steps = -5;
	s.min_transmit = 7.0f;
	ve::clamp_transparency_settings(&s);
	CHECK(s.min_step_m >= 0.01f);
	CHECK(s.min_step_m <= 1.0f);
	CHECK(s.max_steps >= 1);
	CHECK(s.max_steps <= 256);
	CHECK(s.min_transmit <= 0.5f);
	CHECK(s.min_transmit >= 0.0f);
}

TEST_CASE("the transparency store round-trips every knob and clamps on the way in") {
	ve::TransparencySettingsStore store;
	CHECK(store.set_value("enabled", 0.0f));
	CHECK(store.get().enabled == false);
	CHECK(store.set_value("min_step_m", 0.2f));
	CHECK(store.value("min_step_m") == doctest::Approx(0.2f));
	CHECK(store.set_value("max_steps", 1e9f));
	CHECK(store.value("max_steps") <= 256.0f);
	CHECK(store.set_value("min_transmit", 0.05f));
	CHECK(store.value("min_transmit") == doctest::Approx(0.05f));
	CHECK_FALSE(store.set_value("no_such_knob", 1.0f));
}
