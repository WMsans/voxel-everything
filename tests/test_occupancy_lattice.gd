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

# A thin carved slot between two walls is the case the 27-sample probe cannot see: every
# probe sample lands in rock, the lattice says otherwise, and the difference is a phantom
# anchor that keeps a severed piece hanging in the air.
func test_thin_carve_is_air_in_the_occupancy_grid(timeout := 60000) -> void:
	var w := make_world()
	var c := Vector3(24.4, 51.2, 24.4)
	w.debug_apply_sphere_subtract(c, 0.35) # smaller than the probe's 0.4 m sample spacing
	w.debug_stream_region(Vector3i(0, 2, 0))
	for i in range(16):
		w.debug_stream_frame(c)
	w.debug_pump_occupancy() # drain the readback ring into the grid
	var cell := Vector3i(int(floor(c.x / 0.8)), int(floor(c.y / 0.8)), int(floor(c.z / 0.8)))
	var state := int(w.debug_occupancy_state(cell))
	assert_int(state).is_not_equal(0) # the streamed cell is known
	assert_int(state).is_not_equal(3) # not kCellFull

# The exactness claim, stated as a diff: for every resident brick of a region, the GPU's
# occupancy byte equals ve::cell_state_field on the same field.
func test_gpu_occupancy_matches_the_cpu_rule(timeout := 60000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	w.debug_pump_occupancy()
	var d: Dictionary = w.debug_occupancy_diff(Vector3i(0, 2, 0))
	assert_int(int(d["compared"])).is_greater(100)
	assert_int(int(d["mismatches"])).is_equal(0)
