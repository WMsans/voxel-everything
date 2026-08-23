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

# A thin carved slot between two walls is deliberately centered between the 27 probe
# samples: the lattice sees air in this otherwise solid brick, while the probe sees rock.
func test_thin_carve_between_probe_samples_is_solid_in_the_occupancy_grid(timeout := 60000) -> void:
	var w := make_world()
	var cell := Vector3i(25, 62, 25)
	var c := Vector3(cell) * 0.8 + Vector3(0.2, 0.2, 0.2)
	w.hooks().debug_apply_sphere_subtract(c, 0.25) # nearest 0.4 m probe sample is 0.346 m away
	w.hooks().debug_stream_region(Vector3i(0, 1, 0))
	for i in range(16):
		w.hooks().debug_stream_frame(c)
	w.hooks().debug_pump_occupancy() # drain the readback ring into the grid
	var state := int(w.hooks().debug_occupancy_state(cell))
	assert_int(w.hooks().debug_cell_state(cell)).is_equal(2)
	assert_int(state).is_equal(2) # kCellSolid: the 5 cm lattice contains both rock and carve
	assert_int(state).is_equal(w.hooks().debug_cell_state(cell))

# The exactness claim, stated as a diff: for every resident brick of a region, the GPU's
# occupancy byte equals ve::cell_state_field on the same field.
func test_gpu_occupancy_matches_the_cpu_rule(timeout := 60000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	w.hooks().debug_pump_occupancy()
	var d: Dictionary = w.hooks().debug_occupancy_diff(Vector3i(0, 2, 0))
	assert_int(int(d["compared"])).is_greater(100)
	assert_int(int(d["mismatches"])).is_equal(0)

# A plain stream-in has no generated lattice for probe-missed bricks. Its occupancy must
# come from the unambiguous 27-sample fallback, and the helper must run mark directly before
# any generation so this does not accidentally compare only generated bricks.
func test_no_surface_fallback_matches_cell_state_probe(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_occupancy_fallback_diff(Vector3i(0, 2, 0))
	assert_int(int(d["fallback"])).is_greater(100)
	assert_int(int(d["compared"])).is_equal(int(d["fallback"]))
	assert_int(int(d["mismatches"])).is_equal(0)
