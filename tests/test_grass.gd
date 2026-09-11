extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction every other GPU suite in this repo uses (see tests/test_ssao.gd):
# a local rendering device, physics off, streamed until the chunk queue goes quiet.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_grass_pass_runs_and_reports_its_capacity() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["ran"]).is_true()
	assert_int(d["capacity"]).is_greater(0)
	# Nothing is placed yet -- Task 5 fills the cull in, Task 6 the placement.
	assert_int(d["blades"]).is_greater_equal(0)

func test_grass_settings_round_trip_through_the_store() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 25.0)
	assert_float(w.get_grass_value("reach_m")).is_equal_approx(25.0, 0.001)
	# Out-of-range values are clamped by the store, not rejected.
	w.set_grass_value("reach_m", 1.0e9)
	assert_float(w.get_grass_value("reach_m")).is_less_equal(256.0)

func test_disabling_grass_zeroes_the_pass() -> void:
	var w := make_world()
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["bricks"]).is_equal(0)
	assert_int(d["blades"]).is_equal(0)

func test_stage_one_finds_surface_bricks_under_the_camera() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	# Standing on terrain at (30, 56.2, 30) with a 40 m reach, the brick box must contain
	# resident surface bricks. Zero here means the cull rejected everything.
	assert_int(d["bricks"]).is_greater(0)

func test_stage_one_finds_nothing_far_above_the_world() -> void:
	var w := make_world()
	# Stream around a point 2 km up: nothing is resident within the vertical reach.
	for i in range(30):
		w.hooks().debug_stream_frame(Vector3(30.0, 2000.0, 30.0))
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["bricks"]).is_equal(0)

func test_a_shorter_reach_culls_more_bricks() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 40.0)
	var wide: int = w.hooks().debug_grass_stats()["bricks"]
	w.set_grass_value("reach_m", 5.0)
	var narrow: int = w.hooks().debug_grass_stats()["bricks"]
	assert_int(narrow).is_less(wide)
