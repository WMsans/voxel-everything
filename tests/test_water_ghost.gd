extends GdUnitTestSuite

# Ghost water on the GPU (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3): the
# occupancy grid, the collision lattice and island extraction read the SOLID view, in which a
# liquid is air. Each GPU product is diffed against its CPU reference with water present,
# and checked against the claim itself.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(physics := false) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	if physics:
		assert_bool(w.hooks().debug_init_physics()).is_true()
	else:
		w.ensure_initialized()
	return w

func material_id(w: VoxelWorld, name: String) -> int:
	for m in w.material_table():
		if m["name"] == name:
			return m["id"]
	return 0

# A point in region (0, 2, 0), well above the meadow: cell (30, 84, 30).
const SKY := Vector3(24.4, 67.6, 24.4)

func stream_and_pump(w: VoxelWorld, at: Vector3) -> void:
	w.hooks().debug_stream_region(Vector3i(0, 2, 0))
	for i in range(16):
		w.hooks().debug_stream_frame(at)
	w.hooks().debug_pump_occupancy()

func test_a_floating_water_ball_is_air_in_the_occupancy_grid(timeout := 60000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "water"))
	stream_and_pump(w, SKY)
	var cell := Vector3i(30, 84, 30)
	assert_int(w.hooks().debug_cell_state(cell)).is_equal(1)     # kCellAir, the CPU rule
	assert_int(int(w.hooks().debug_occupancy_state(cell))).is_equal(1)

func test_a_floating_ice_ball_stays_full_in_the_occupancy_grid(timeout := 60000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "ice"))
	stream_and_pump(w, SKY)
	var cell := Vector3i(30, 84, 30)
	assert_int(w.hooks().debug_cell_state(cell)).is_equal(3)     # kCellFull
	assert_int(int(w.hooks().debug_occupancy_state(cell))).is_equal(3)

func test_gpu_occupancy_matches_the_cpu_rule_with_water_in_the_region(timeout := 60000) -> void:
	var w := make_world()
	var water := material_id(w, "water")
	w.hooks().debug_apply_sphere_add(SKY, 1.5, water)
	# Water over the meadow too: the bricks there hold ground and water, so the gated second
	# evaluation in brick_gen runs on bricks that also have a real surface.
	w.hooks().debug_apply_sphere_add(Vector3(24.4, 51.4, 24.4), 1.5, water)
	stream_and_pump(w, Vector3(24.4, 51.4, 24.4))
	var d: Dictionary = w.hooks().debug_occupancy_diff(Vector3i(0, 2, 0))
	assert_int(int(d["compared"])).is_greater(100)
	assert_int(int(d["mismatches"])).override_failure_message(
		"GPU occupancy disagrees with cell_state_field: %s" % d).is_equal(0)

# Chunk (3, 10, 3) spans y [64, 70.4) above the meadow: no terrain surface.
const SKY_CHUNK := Vector3i(3, 10, 3)

func test_a_water_ball_puts_no_surface_in_the_collision_lattice(timeout := 60000) -> void:
	var w := make_world(true)
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "water"))
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(SKY_CHUNK)
	assert_int(int(d["max_diff"])).is_less_equal(1)
	assert_bool(d["has_surface"]).override_failure_message(
		"the collision lattice sees the water ball: %s" % d).is_false()

func test_a_rock_ball_does_put_a_surface_in_the_collision_lattice(timeout := 60000) -> void:
	# The control: the same ball in rock is a collider, so the test above can see a ball at all.
	var w := make_world(true)
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "rock"))
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(SKY_CHUNK)
	assert_int(int(d["max_diff"])).is_less_equal(1)
	assert_bool(d["has_surface"]).is_true()

# Cells covering a rock ball at SKY and a water cap above it.
const ISLAND_LO := Vector3i(29, 83, 29)
const ISLAND_HI := Vector3i(31, 87, 31)

func test_island_extraction_strips_water_and_matches_the_cpu(timeout := 60000) -> void:
	var rock_world := make_world(true)
	rock_world.hooks().debug_apply_sphere_add(SKY, 1.0, material_id(rock_world, "rock"))
	var a: Dictionary = rock_world.hooks().debug_island_extract_diff(ISLAND_LO, ISLAND_HI)
	var wet_world := make_world(true)
	wet_world.hooks().debug_apply_sphere_add(SKY, 1.0, material_id(wet_world, "rock"))
	wet_world.hooks().debug_apply_sphere_add(SKY + Vector3(0, 1.0, 0), 1.0,
			material_id(wet_world, "water"))
	var b: Dictionary = wet_world.hooks().debug_island_extract_diff(ISLAND_LO, ISLAND_HI)
	assert_bool(b.get("ok", false)).is_true()
	assert_int(int(b["worst_steps"])).is_less(2)
	assert_int(int(b["mat_mismatch"])).is_equal(0)
	assert_int(int(b["gpu_solid"])).is_equal(int(b["cpu_solid"]))
	# The water contributed no solid voxel: the island is the rock alone.
	assert_int(int(b["gpu_solid"])).override_failure_message(
		"the island carries water: %s vs rock alone %s" % [b, a]).is_equal(int(a["gpu_solid"]))
