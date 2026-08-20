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
	w.ensure_initialized()
	return w

# The bake has to reproduce the field it replaces. debug_consolidate_diff bakes one region's
# bricks on the worker device, then compares every baked lattice sample against ve::eval_field
# with the same ops -- the same shape as debug_brick_diff, one level up.
func test_bake_reproduces_the_field(timeout := 60000) -> void:
	var w := make_world()
	for i in range(12):
		w.debug_apply_sphere_subtract(Vector3(24.0 + float(i) * 0.5, 51.5, 24.0), 1.2)
	var d: Dictionary = w.debug_consolidate_diff(Vector3i(0, 2, 0))
	assert_int(int(d["bricks"])).is_greater(0)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)

# An overridden brick must read back through the FIELD, not just out of the pool: the whole
# point is that every consumer sees the same base.
func test_overridden_brick_reads_through_eval_field(timeout := 60000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	# The op list is gone; the crater must still be there.
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)
	var hit: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	assert_float(float(hit["pos"].y)).is_less(51.4 - 1.0)
