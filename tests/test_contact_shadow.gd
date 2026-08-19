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
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_mask_is_half_resolution() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_int(d["mask_width"]).is_equal(64)
	assert_int(d["mask_height"]).is_equal(64)

# The generator carves a 5 m sphere out of the terrain at (30, ~49, 30). Looking into it,
# the crater walls occlude their own floor over a short distance -- exactly the geometry
# contact shadows exist for.
func test_a_crater_darkens_its_own_floor() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(d["mask_min"]).override_failure_message(
		"no pixel was occluded at all: the march never hit anything").is_less(0.9)
	assert_float(d["mask_mean"]).is_between(0.0, 1.0)
	# ...and it did not darken EVERYTHING, which is what a sign error in the march produces.
	assert_float(d["mask_mean"]).is_greater(0.3)

func test_the_apply_only_ever_darkens() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(d["max_brightening"]).override_failure_message(
		"a contact shadow made a pixel brighter").is_less(0.002)
	assert_float(d["mean_darkening"]).is_greater(0.0)

func test_turning_contact_shadows_off_leaves_the_image_alone() -> void:
	var w := make_world()
	w.set_effect_enabled("contact_shadows", false)
	var d: Dictionary = w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(d["mean_darkening"]).is_equal_approx(0.0, 0.001)

func test_zero_steps_reads_as_off_rather_than_as_a_free_dispatch() -> void:
	var w := make_world()
	w.quality_tier = 0
	var d: Dictionary = w.debug_beauty_settings()
	assert_int(int(d["flags"]) & 4).is_equal(0)
