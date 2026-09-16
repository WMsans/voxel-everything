extends GdUnitTestSuite
# Characterization for docs/superpowers/plans/2026-09-16-settings-store.md (Task 2): every name
# VoxelWorld's settings API accepts today, round-tripped through that API. The store migration
# must keep every name and its meaning; this suite stays green through the whole plan.

const BEAUTY_SWITCHES := ["ssgi", "ssr", "contact_shadows", "outlines", "sun_shadow_map",
	"glossy_sdf_rays", "raymarched_sun_shadow", "ssao", "cost_view"]
# [name, an in-range value that differs from the High default]
const BEAUTY_MAGNITUDES := [["ssgi_radius", 12.0], ["ssgi_temporal", 0.5], ["ssgi_strength", 2.0],
	["ssao_radius", 8.0], ["ssao_strength", 1.0], ["emissive_gi_radius", 32.0], ["emissive_gi_strength", 3.0],
	["outline_depth_threshold", 0.1], ["outline_normal_threshold", 0.5], ["outline_darken", 0.5],
	["contact_reach_m", 1.0], ["contact_strength", 0.5], ["contact_bias_m", 0.1]]
const GRASS := [["enabled", 0.0], ["reach_m", 25.0], ["vertical_reach_m", 5.0],
	["blades_per_brick", 8.0], ["max_blades", 1000.0], ["blade_width_m", 0.05],
	["blade_height_m", 0.5], ["height_jitter", 0.2], ["slope_cos_min", 0.3],
	["wind_strength", 0.2], ["wind_speed", 1.0], ["wind_scale", 0.1], ["wind_dir_deg", 90.0],
	["lean_spread_rad", 1.0], ["base_curve", 0.3], ["camera_tilt", 0.5],
	["ring_width_gain", 2.0], ["flower_chance", 0.05], ["gloss", 0.5], ["blade_lighting", 0.3]]

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	return w

func test_every_beauty_switch_round_trips_by_name() -> void:
	var w := make_world()
	for name in BEAUTY_SWITCHES:
		var before: bool = w.get_effect_enabled(name)
		w.set_effect_enabled(name, not before)
		assert_bool(w.get_effect_enabled(name)).override_failure_message(name).is_equal(not before)

func test_every_beauty_magnitude_round_trips_by_name() -> void:
	var w := make_world()
	for entry in BEAUTY_MAGNITUDES:
		w.set_effect_value(entry[0], entry[1])
		assert_float(w.get_effect_value(entry[0])).override_failure_message(entry[0]) \
			.is_equal_approx(entry[1], 0.0001)

func test_the_render_dials_round_trip() -> void:
	var w := make_world()
	for name in ["islands", "near_field"]:
		w.set_effect_enabled(name, false)
		assert_bool(w.get_effect_enabled(name)).override_failure_message(name).is_false()
	w.near_field_scale = 0.55
	assert_float(w.near_field_scale).is_equal_approx(0.55, 0.0001)
	w.quality_tier = 1
	assert_int(w.quality_tier).is_equal(1)

func test_every_grass_knob_round_trips_by_name() -> void:
	var w := make_world()
	for entry in GRASS:
		assert_bool(w.set_grass_value(entry[0], entry[1])).override_failure_message(entry[0]).is_true()
		assert_float(w.get_grass_value(entry[0])).override_failure_message(entry[0]) \
			.is_equal_approx(entry[1], 0.0001)

# Settings-store plan Task 8: the counts became settable by name.
const BEAUTY_COUNTS := [["ssgi_taps", 4], ["ssr_steps", 12], ["contact_steps", 8],
	["ssao_steps", 4], ["ssao_directions", 4]]

func test_every_beauty_count_is_settable_by_name() -> void:
	var w := make_world()
	for entry in BEAUTY_COUNTS:
		w.set_effect_value(entry[0], entry[1])
		assert_float(w.get_effect_value(entry[0])).override_failure_message(entry[0]) \
			.is_equal_approx(float(entry[1]), 0.0001)
		assert_int(int(w.hooks().debug_beauty_settings()[entry[0]])).is_equal(entry[1])
	# A switch is not a magnitude: set_effect_value leaves it alone.
	w.set_effect_value("ssgi", 0.0)
	assert_bool(w.get_effect_enabled("ssgi")).is_true()
