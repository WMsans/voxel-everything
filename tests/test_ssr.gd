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
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(20, 56.2, 20)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_reflection_target_is_half_resolution() -> void:
	var d: Dictionary = make_world().debug_ssr_probe(0, 321, 181)
	assert_int(d["width"]).is_equal(160)
	assert_int(d["height"]).is_equal(90)

# Fixture 0 has a mirror receiver in both depth buffers and a red blocker written ONLY to
# post-opaque scene colour/depth. A hit proves SSR reads merged scene depth.
func test_a_post_opaque_only_blocker_is_reflected() -> void:
	var d: Dictionary = make_world().debug_ssr_probe(0, 128, 128)
	assert_bool(d["ran"]).is_true()
	assert_int(d["steps"]).is_equal(24)
	assert_int(d["hit_pixels"]).is_greater(0)
	assert_float(d["red_gain"]).is_greater(0.01)
	assert_float(d["max_weight"]).is_between(0.0, 0.85)

func test_removing_the_scene_only_blocker_removes_its_hits() -> void:
	var w := make_world()
	var a: Dictionary = w.debug_ssr_probe(0, 128, 128)
	var b: Dictionary = w.debug_ssr_probe(1, 128, 128)
	assert_int(b["hit_pixels"]).is_less(int(a["hit_pixels"]))

func test_medium_uses_twelve_steps_and_off_dispatches_nothing() -> void:
	var w := make_world()
	w.quality_tier = 2
	assert_int(w.debug_ssr_probe(0, 128, 128)["steps"]).is_equal(12)
	w.quality_tier = 0
	var off: Dictionary = w.debug_ssr_probe(0, 128, 128)
	assert_bool(off["ran"]).is_false()
	assert_float(off["mean_delta"]).is_equal_approx(0.0, 0.0001)

func test_the_apply_is_bounded_and_preserves_alpha() -> void:
	var d: Dictionary = make_world().debug_ssr_probe(0, 128, 128)
	assert_float(d["max_weight"]).is_less_equal(0.85)
	assert_float(d["max_alpha_delta"]).is_less(0.0001)
	assert_bool(d["finite"]).is_true()

# Fixtures 2 and 3 contain the same dynamic receiver and scene blocker, but the optional
# normal-roughness texture has alpha 0 and 1 respectively. Dynamic pixels must be skipped;
# voxel receivers outside the dynamic region must still reflect the scene blocker.
func test_uncalibrated_normal_roughness_does_not_enable_dynamic_receivers() -> void:
	var w := make_world()
	var alpha_zero: Dictionary = w.debug_ssr_probe(2, 128, 128)
	var alpha_one: Dictionary = w.debug_ssr_probe(3, 128, 128)
	assert_bool(alpha_zero["ran"]).is_true()
	assert_bool(alpha_one["ran"]).is_true()
	assert_int(alpha_zero["dynamic_hit_pixels"]).is_equal(0)
	assert_int(alpha_one["dynamic_hit_pixels"]).is_equal(0)
	assert_int(alpha_zero["scene_hit_pixels"]).is_greater(0)
	assert_int(alpha_one["scene_hit_pixels"]).is_greater(0)
	assert_float(alpha_zero["mean_delta"]).is_equal_approx(float(alpha_one["mean_delta"]), 0.0001)
	assert_int(alpha_zero["hit_pixels"]).is_equal(int(alpha_one["hit_pixels"]))

# Current roughness assets never exceed gloss 0.5. The probe uses a negative params.w
# sentinel to force shader gloss=1 on a real ground hit without changing production data.
func test_true_sdf_reflections_are_high_only_and_change_only_albedo() -> void:
	var w := make_world()
	var origin := Vector3(20, 75, 20)
	var dir := Vector3(0, -1, 0)
	w.quality_tier = 2
	var medium: Dictionary = w.debug_glossy_sdf_probe(origin, dir)
	w.quality_tier = 3
	var high: Dictionary = w.debug_glossy_sdf_probe(origin, dir)
	assert_bool(high["hit"]).is_true()
	var ha: Color = high["albedo"]
	var ma: Color = medium["albedo"]
	var albedo_delta := maxf(absf(ha.r - ma.r), maxf(absf(ha.g - ma.g), absf(ha.b - ma.b)))
	assert_float(albedo_delta).is_greater(0.005)
	assert_int(high["material"]).is_equal(int(medium["material"]))
	assert_float(high["gloss"]).is_equal_approx(float(medium["gloss"]), 0.001)
	assert_float(high["sun"]).is_equal_approx(float(medium["sun"]), 0.01)
	assert_float(Vector3(high["position"]).distance_to(Vector3(medium["position"]))).is_less(0.01)
