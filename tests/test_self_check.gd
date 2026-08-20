extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func test_self_check_reports_zero_mismatches_on_a_clean_world(timeout := 120000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	var d: Dictionary = w.debug_self_check()
	assert_bool(d["ok"]).is_true()
	for key in ["field_mismatches", "brick_mismatches", "mesh_mismatches",
			"lod_mismatches", "occupancy_mismatches"]:
		assert_int(int(d[key])).is_equal(0)
