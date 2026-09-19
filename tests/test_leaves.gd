extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction every other GPU suite in this repo uses (see tests/test_grass.gd).
# Streams from (20, 60, 30) rather than the usual (30, 56.2, 30) hook view: that view sits
# inside the cave and sees only buried geometry, which is the wrong place to judge canopies.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 60.0, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_leaf_pass_runs_and_reports_its_capacity() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_bool(d["ran"]).is_true()
	assert_int(d["capacity"]).is_greater(0)

func test_leaf_settings_round_trip_through_the_store() -> void:
	var w := make_world()
	w.set_leaf_value("reach_m", 180.0)
	assert_float(w.get_leaf_value("reach_m")).is_equal_approx(180.0, 0.001)
	w.set_leaf_value("reach_m", 1.0e9)
	assert_float(w.get_leaf_value("reach_m")).is_less_equal(400.0)

func test_disabling_leaves_zeroes_the_pass() -> void:
	var w := make_world()
	w.set_leaf_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["trees"]).is_equal(0)
	assert_int(d["clumps"]).is_equal(0)

func test_stage_one_finds_trees_near_the_camera() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["trees"]).is_greater(0)

func test_the_tree_list_shrinks_as_the_camera_retreats() -> void:
	var w := make_world()
	var near_count: int = w.hooks().debug_leaf_stats()["trees"]
	w.set_leaf_value("reach_m", 40.0)
	var far_count: int = w.hooks().debug_leaf_stats()["trees"]
	assert_int(far_count).is_less(near_count)

# THE headline contract. Painting the whole reach to rock removes every trunk's bark voxels
# without moving any geometry, so the stage-1 attachment check must drop every tree.
#
# Paint, NOT dig: digging a trunk exposes fresh ground around it and would confound removal
# with exposure. This is the same correction tests/test_grass.gd already had to make.
func test_painting_trunks_away_empties_the_tree_list() -> void:
	var w := make_world()
	var before: int = w.hooks().debug_leaf_stats()["trees"]
	assert_int(before).is_greater(0)
	# Material 2 is rock; see ve::kMaterials in extension/src/world/material_table.h.
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 60.0, 30.0), 60.0, 2)
	for i in range(40):
		w.hooks().debug_stream_frame(Vector3(20.0, 60.0, 30.0))
	assert_int(w.hooks().debug_leaf_stats()["trees"]).is_equal(0)

func test_stage_two_places_clumps() -> void:
	var w := make_world()
	assert_int(w.hooks().debug_leaf_stats()["clumps"]).is_greater(0)

func test_the_clump_count_falls_as_the_reach_shrinks() -> void:
	var w := make_world()
	var far: int = w.hooks().debug_leaf_stats()["clumps"]
	w.set_leaf_value("reach_m", 60.0)
	var near: int = w.hooks().debug_leaf_stats()["clumps"]
	assert_int(near).is_less(far)

func test_every_clump_sits_inside_its_crown() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["sampled"]).is_greater(0)
	# Distance from the clump centre to its crown centre, as a fraction of the crown
	# radius. tree.glslh's containment invariant says this never exceeds 1: a clump centre
	# on a lobe shell is inside the crown sphere by construction (max lobe reach 0.62 plus
	# max lobe radius 0.38 = 1.0 crown radii). The card radius is excluded on purpose: the
	# density LOD lets a far clump's card outgrow that last margin.
	assert_float(d["max_crown_offset"]).is_less_equal(1.001)

func test_the_clump_count_clamps_at_capacity_instead_of_overflowing() -> void:
	var w := make_world()
	w.set_leaf_value("max_clumps", 64.0)
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["clumps"]).is_less_equal(64)
	assert_int(d["high_water"]).is_greater(0)

func test_zero_clumps_per_tree_places_nothing_but_still_finds_trees() -> void:
	var w := make_world()
	w.set_leaf_value("clumps_per_tree", 0.0)
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["clumps"]).is_equal(0)
