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
	# Nothing is placed yet.
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

func test_blades_appear_on_grass_terrain() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_greater(0)

func test_blade_count_falls_as_the_camera_retreats() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 40.0)
	var near_stats: Dictionary = w.hooks().debug_grass_stats()
	var near_count: int = near_stats["blades"]
	w.set_grass_value("reach_m", 12.0)
	var far_stats: Dictionary = w.hooks().debug_grass_stats()
	assert_int(far_stats["blades"]).is_less(near_count)

func test_density_follows_blades_per_brick() -> void:
	var w := make_world()
	w.set_grass_value("blades_per_brick", 16.0)
	var dense_stats: Dictionary = w.hooks().debug_grass_stats()
	var dense: int = dense_stats["blades"]
	w.set_grass_value("blades_per_brick", 4.0)
	var sparse_stats: Dictionary = w.hooks().debug_grass_stats()
	assert_int(sparse_stats["blades"]).is_less(dense)

func test_the_blade_count_clamps_at_capacity_instead_of_overflowing() -> void:
	var w := make_world()
	w.set_grass_value("max_blades", 64.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_less_equal(64)
	# The high-water mark still reports what the frame WANTED, so an overflow is visible
	# rather than silent.
	assert_int(d["high_water"]).is_greater_equal(d["blades"])

# Grass refuses steep surfaces. Raising the threshold past vertical must leave nothing.
func test_no_blades_survive_an_impossible_slope_threshold() -> void:
	var w := make_world()
	w.set_grass_value("slope_cos_min", 1.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_equal(0)

# The spec's placement contract: no blade may stand on a surface steeper than the slope
# threshold allows, and no blade may be taller than the settings permit.
func test_every_sampled_blade_stands_on_an_up_facing_surface() -> void:
	var w := make_world()
	w.set_grass_value("slope_cos_min", 0.55)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["sampled"]).is_greater(0)
	assert_float(d["min_normal_y"]).is_greater_equal(0.5)  # 0.55 less oct quantisation
	assert_float(d["max_height"]).is_less_equal(
		w.get_grass_value("blade_height_m") * (1.0 + w.get_grass_value("height_jitter")) * 1.15 + 0.001)

func test_the_raster_issues_three_vertices_per_blade() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["drawn"]).is_true()
	assert_int(d["vertices"]).is_equal(d["blades"] * 3)

func test_no_blades_means_no_draw_but_not_a_failure() -> void:
	var w := make_world()
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["vertices"]).is_equal(0)
	assert_bool(d["ran"]).is_true()

# The edit-awareness contract from the design doc: the scatter reads the LIVE atlas, so an
# edit that takes grass away must take its blades on the next frame, with no invalidation
# code anywhere. Tested rather than assumed. This paints the whole reach to rock instead
# of digging a crater: a crater floor is fresh grass-band surface and legitimately grows
# NEW blades (74 -> 409 measured), which confounds removal with exposure. Painting moves
# only the material layer, so zero blades afterwards can only mean the scatter read it.
# (Hook name/signature verbatim from extension/src/debug/hooks.cpp:
# debug_apply_sphere_paint(centre, radius, material); rock is material id 2.)
func test_painting_grass_to_rock_removes_its_blades() -> void:
	var w := make_world()
	var before: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(before).is_greater(0)
	w.hooks().debug_apply_sphere_paint(Vector3(30.0, 50.0, 30.0), 45.0, 2)
	for i in range(60):
		w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0))
	var after: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(after).is_equal(0)

# Disabling grass on a world that already drew must REALLY stop the draw: the disabled
# run() clears the GPU draw args (not just the CPU counters), so the raster issues an
# empty indirect draw instead of re-drawing the previous frame's frozen blades. Same
# world throughout -- a fresh world proves nothing, its buffers read back as zero anyway.
func test_disabling_grass_after_it_drew_issues_an_empty_draw() -> void:
	var w := make_world()
	assert_int(w.hooks().debug_grass_stats()["blades"]).is_greater(0)
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_equal(0)
	assert_int(d["vertices"]).is_equal(0)
	assert_bool(d["drawn"]).is_true()
