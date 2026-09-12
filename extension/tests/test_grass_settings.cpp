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

// Every new shape knob has to survive a hostile value, because these feed the vertex
// shader's arc directly: a negative curve bends blades into the ground and a NaN spread
// takes the whole field with it.
TEST_CASE("the shape knobs clamp into range from hostile values") {
	ve::GrassSettings s;
	s.wind_dir_deg = 1e9f;
	s.lean_spread_rad = -4.0f;
	s.base_curve = 1e9f;
	s.ring_width_gain = -1.0f;
	ve::clamp_grass_settings(&s);
	CHECK(s.wind_dir_deg >= 0.0f);
	CHECK(s.wind_dir_deg <= 360.0f);
	CHECK(s.lean_spread_rad >= 0.0f);
	CHECK(s.base_curve <= 2.0f);
	CHECK(s.ring_width_gain >= 0.0f);

	ve::GrassSettings n;
	n.lean_spread_rad = 0.0f / 0.0f;
	n.base_curve = 0.0f / 0.0f;
	ve::clamp_grass_settings(&n);
	CHECK(n.lean_spread_rad >= 0.0f);
	CHECK(n.base_curve >= 0.0f);
}

TEST_CASE("the store names the shape knobs too") {
	ve::GrassSettingsStore store;
	CHECK(store.set_value("wind_dir_deg", 120.0f));
	CHECK(store.value("wind_dir_deg") == doctest::Approx(120.0f));
	CHECK(store.set_value("lean_spread_rad", 0.4f));
	CHECK(store.value("lean_spread_rad") == doctest::Approx(0.4f));
	CHECK(store.set_value("base_curve", 0.6f));
	CHECK(store.value("base_curve") == doctest::Approx(0.6f));
	CHECK(store.set_value("ring_width_gain", 2.0f));
	CHECK(store.value("ring_width_gain") == doctest::Approx(2.0f));
}

// Camera tilt is the top-down fix: it leans every blade away from the viewer as the camera
// pitches down, which holds the projected blade area a level view enjoys at every angle.
// It is a FRACTION of the camera's elevation, so anything outside 0..1 is either an
// untilted blade -- the bug this exists to fix -- or one that has leaned past flat.
TEST_CASE("camera tilt is a clamped fraction of the camera elevation") {
	ve::GrassSettingsStore store;
	CHECK(store.value("camera_tilt") == doctest::Approx(0.85f));
	CHECK(store.set_value("camera_tilt", 1.0f));
	CHECK(store.value("camera_tilt") == doctest::Approx(1.0f));
	CHECK(store.set_value("camera_tilt", 4.0f));
	CHECK(store.value("camera_tilt") <= 1.0f);
	CHECK(store.set_value("camera_tilt", -2.0f));
	CHECK(store.value("camera_tilt") >= 0.0f);
	// NaN falls through to the conservative end of the clamp rather than into the shader.
	CHECK(store.set_value("camera_tilt", 0.0f / 0.0f));
	CHECK(store.value("camera_tilt") >= 0.0f);
	CHECK(store.value("camera_tilt") <= 1.0f);
}

// Blade lighting is how much of a blade's own rounded normal survives against the ground
// normal. 0 is the flat meadow that landed in one cel band; past 1 is not a blend at all.
TEST_CASE("blade lighting is a clamped blend between ground and blade normals") {
	ve::GrassSettingsStore store;
	CHECK(store.value("blade_lighting") == doctest::Approx(0.6f));
	CHECK(store.set_value("blade_lighting", 0.3f));
	CHECK(store.value("blade_lighting") == doctest::Approx(0.3f));
	CHECK(store.set_value("blade_lighting", 3.0f));
	CHECK(store.value("blade_lighting") <= 1.0f);
	CHECK(store.set_value("blade_lighting", -1.0f));
	CHECK(store.value("blade_lighting") >= 0.0f);
	CHECK(store.set_value("blade_lighting", 0.0f / 0.0f));
	CHECK(store.value("blade_lighting") >= 0.0f);
	CHECK(store.value("blade_lighting") <= 1.0f);
}

// The scatter shader runs one workgroup of 64 threads per brick and every thread is one
// candidate blade, so a default above 64 would silently drop blades on the floor.
TEST_CASE("the default blade density fits the scatter workgroup") {
	ve::GrassSettings s;
	CHECK(s.blades_per_brick <= 64);
}
