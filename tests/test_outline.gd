extends GdUnitTestSuite
var _worlds: Array = []
func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w): w.free()
	_worlds.clear()
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true; w.physics_enabled = false
	add_child(w); _worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w
func test_depth_line_is_one_pixel_and_darkens_by_the_fixed_amount() -> void:
	var d: Dictionary = make_world().debug_outline_probe(1, false)
	assert_int(d["dark_columns"]).is_equal(1)
	assert_float(d["dark_value"]).is_equal_approx(0.35, 0.01)
	assert_float(d["max_brightening"]).is_less(0.0001)
func test_terrain_normal_line_works_at_equal_depth() -> void:
	assert_int(make_world().debug_outline_probe(2, false)["dark_columns"]).is_equal(1)
func test_dynamic_normals_follow_the_spike_verdict() -> void:
	var w := make_world()
	assert_int(w.debug_outline_probe(3, true)["dark_columns"]).is_equal(1)
	assert_int(w.debug_outline_probe(3, false)["dark_columns"]).is_equal(0)
func test_dynamic_depth_line_survives_the_fallback() -> void:
	assert_int(make_world().debug_outline_probe(4, false)["dark_columns"]).is_equal(1)
func test_flat_and_off_are_unchanged() -> void:
	var w := make_world()
	assert_int(w.debug_outline_probe(0, false)["dark_columns"]).is_equal(0)
	w.set_effect_enabled("outlines", false)
	var d: Dictionary = w.debug_outline_probe(1, false)
	assert_bool(d["ran"]).is_false()
	assert_float(d["mean_delta"]).is_equal_approx(0.0, 0.0001)
	assert_float(d["max_alpha_delta"]).is_less(0.0001)
