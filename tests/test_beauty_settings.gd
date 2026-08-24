extends GdUnitTestSuite

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
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	return w

func test_the_default_tier_is_high() -> void:
	var w := make_world()
	assert_int(w.quality_tier).is_equal(3)
	var d := w.hooks().debug_beauty_settings()
	assert_int(d["ssgi_taps"]).is_equal(8)
	assert_bool(d["outlines"]).is_true()

func test_setting_the_tier_replaces_every_knob() -> void:
	var w := make_world()
	w.quality_tier = 0
	var d := w.hooks().debug_beauty_settings()
	assert_bool(d["ssgi"]).is_false()
	assert_bool(d["ssr"]).is_false()
	assert_bool(d["outlines"]).is_false()
	assert_int(d["flags"]).is_equal(0)

func test_individual_effects_toggle_by_name() -> void:
	var w := make_world()
	w.set_effect_enabled("outlines", false)
	assert_bool(w.get_effect_enabled("outlines")).is_false()
	assert_bool(w.get_effect_enabled("ssr")).is_true()
	# Bit 8 is kFlagOutlines; clearing it must not disturb the others.
	var d := w.hooks().debug_beauty_settings()
	assert_int(int(d["flags"]) & 8).is_equal(0)
	assert_int(int(d["flags"]) & 2).is_equal(2)

func test_ssao_is_on_at_the_default_tier() -> void:
	var w := make_world()
	assert_bool(w.hooks().debug_beauty_settings()["ssao"]).is_true()

# HBAO costs a full-screen pass: it joins SSGI/SSR/contact shadows behind the Medium gate
# rather than riding along with Low's "the image reads as this engine" set.
func test_ssao_needs_medium_quality_or_better() -> void:
	var w := make_world()
	w.quality_tier = 1
	assert_bool(w.hooks().debug_beauty_settings()["ssao"]).is_false()
	w.quality_tier = 2
	assert_bool(w.hooks().debug_beauty_settings()["ssao"]).is_true()
	w.quality_tier = 0
	var d := w.hooks().debug_beauty_settings()
	assert_bool(d["ssao"]).is_false()
	assert_int(d["flags"]).is_equal(0)

# Bit 9 (256) is kFlagSsao; clearing it must not disturb the others.
func test_ssao_toggles_by_name_without_disturbing_other_bits() -> void:
	var w := make_world()
	assert_bool(w.get_effect_enabled("ssao")).is_true()
	assert_int(int(w.hooks().debug_beauty_settings()["flags"]) & 256).is_equal(256)
	w.set_effect_enabled("ssao", false)
	assert_bool(w.get_effect_enabled("ssao")).is_false()
	assert_bool(w.get_effect_enabled("ssgi")).is_true()
	var d := w.hooks().debug_beauty_settings()
	assert_int(int(d["flags"]) & 256).is_equal(0)
	assert_int(int(d["flags"]) & 1).is_equal(1)

func test_an_unknown_effect_name_is_ignored_rather_than_crashing() -> void:
	var w := make_world()
	var before: int = w.hooks().debug_beauty_settings()["flags"]
	w.set_effect_enabled("no_such_effect", false)
	assert_int(w.hooks().debug_beauty_settings()["flags"]).is_equal(before)
	assert_bool(w.get_effect_enabled("no_such_effect")).is_false()
