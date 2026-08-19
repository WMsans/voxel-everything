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
	assert_bool(w.debug_init_atlas()).is_true()
	return w

func test_the_gbuffer_allocates_at_the_requested_size() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_gbuffer_stats(320, 180)
	assert_bool(d["valid"]).is_true()
	assert_int(d["width"]).is_equal(320)
	assert_int(d["height"]).is_equal(180)
	assert_bool(d["albedo_valid"]).is_true()
	assert_bool(d["surface_valid"]).is_true()
	assert_bool(d["depth_valid"]).is_true()
	assert_bool(d["lit_valid"]).is_true()
	assert_bool(d["history_valid"]).is_true()

# The GI history is deliberately half resolution: SSGI reads it at half resolution and a
# full-resolution copy would cost 4x the bandwidth to be downsampled on read anyway.
func test_the_history_is_half_resolution_and_rounds_up_from_odd_sizes() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_gbuffer_stats(321, 181)
	assert_int(d["half_width"]).is_equal(160)
	assert_int(d["half_height"]).is_equal(90)
	var tiny: Dictionary = w.debug_gbuffer_stats(1, 1)
	assert_int(tiny["half_width"]).is_equal(1)
	assert_int(tiny["half_height"]).is_equal(1)

func test_re_ensuring_at_the_same_size_reuses_the_same_textures() -> void:
	var w := make_world()
	var a: Dictionary = w.debug_gbuffer_stats(256, 144)
	var b: Dictionary = w.debug_gbuffer_stats(256, 144)
	assert_int(b["albedo_id"]).is_equal(int(a["albedo_id"]))
	assert_int(b["depth_id"]).is_equal(int(a["depth_id"]))
	assert_int(b["reallocations"]).is_equal(int(a["reallocations"]))

func test_a_different_size_reallocates() -> void:
	var w := make_world()
	var a: Dictionary = w.debug_gbuffer_stats(256, 144)
	var b: Dictionary = w.debug_gbuffer_stats(512, 288)
	assert_int(b["width"]).is_equal(512)
	assert_int(b["reallocations"]).is_greater(int(a["reallocations"]))

func test_a_degenerate_size_is_refused_rather_than_allocated() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_gbuffer_stats(0, 180)
	assert_bool(d["valid"]).is_false()
