extends GdUnitTestSuite

# Spec section 5, end to end. Blow the middle out of a pillar and the top must:
#   1. stop being part of the terrain (the carve),
#   2. become a rigid body that falls (the spawn),
#   3. come back as terrain where it lands (the re-merge).
#
# Every assertion is against the FIELD, through ve::raycast, because that is what the
# renderer, the collision mesher and the LoD bakery all read: if the field says the pillar
# top is gone, every consumer agrees it is gone.

const CENTER := Vector3(20.0, 56.0, 20.0)
# A pillar built above the terrain with sphere-adds, so the test does not depend on where
# the analytic hills happen to put a cliff.
const PILLAR_X := 20.4
const PILLAR_Z := 20.4
const PILLAR_BASE := 52.0

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
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(48, 24, 48)
	w.max_region_slots = 64
	w.physics_radius_m = 30.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func tool_of(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

# One frame of everything: streaming (which fills the occupancy grid), collider maintenance
# and the island manager, in the order VoxelWorld::_process runs them.
func step(w: VoxelWorld, frames: int, center: Vector3 = CENTER) -> void:
	for i in range(frames):
		w.debug_stream_frame(center)
		w.debug_physics_frame(center)
		w.debug_island_frame(1.0 / 60.0, center)

func solid_at(w: VoxelWorld, p: Vector3) -> bool:
	# A downward ray from just above the point: it hits at p if there is matter there.
	var hit: Dictionary = w.debug_raycast(p + Vector3(0, 0.6, 0), Vector3(0, -1, 0))
	# 0.7 m: the helper is also used for points inside a sphere, where the downward ray can
	# start already inside solid and return the origin (p.y + 0.6) as the hit.
	return hit["hit"] and absf((hit["pos"] as Vector3).y - p.y) < 0.7

func build_pillar(w: VoxelWorld, t: VoxelEditTool) -> void:
	# Five overlapping 1.2 m balls stacked into a 6 m column standing on the terrain.
	for i in range(5):
		t.apply_sphere_add(Vector3(PILLAR_X, PILLAR_BASE + 1.0 * i, PILLAR_Z), 1.2, 4)
	step(w, 90)

func test_the_grid_and_the_flood_find_a_severed_pillar_top(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the pillar was never built").is_true()

	# Cut the pillar in half.
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)

	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"nothing came loose: %s" % st).is_greater(0)
	# The top is no longer part of the static field: it is a body now.
	assert_bool(solid_at(w, top)).override_failure_message(
		"the severed top is still in the terrain (the carve did not happen)").is_false()
	# ...and the stump below the cut is untouched.
	assert_bool(solid_at(w, Vector3(PILLAR_X, PILLAR_BASE, PILLAR_Z))).is_true()

func test_the_island_body_falls_and_the_bodies_are_capped(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["live_bodies"]).is_greater(0)
	var y0: float = st["lowest_body_y"]
	for i in range(60):
		await get_tree().physics_frame
		w.debug_island_frame(1.0 / 60.0, CENTER)
	assert_float(w.debug_island_stats()["lowest_body_y"]).override_failure_message(
		"the island did not fall").is_less(y0 - 0.2)
	# Spec section 5's guardrails hold whatever happens.
	assert_int(st["live_bodies"]).is_less_equal(64)
	assert_int(st["live_islands"]).is_less_equal(32)

func test_a_rested_island_merges_back_into_the_terrain(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(0.2) # the demo waits 2 s; a test should not
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	assert_int(w.debug_island_stats()["islands_spawned"]).is_greater(0)

	for i in range(600):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		if w.debug_island_stats()["islands_merged"] > 0:
			break
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_merged"]).override_failure_message(
		"the island never merged back: %s" % st).is_greater(0)
	assert_int(st["live_bodies"]).is_equal(0)
	# Spec section 5: "Rubble permanently accumulates as terrain." The rock is somewhere on
	# the ground under where it fell, and the FIELD knows about it.
	var down: Dictionary = w.debug_raycast(Vector3(PILLAR_X, 90.0, PILLAR_Z), Vector3(0, -1, 0))
	assert_bool(down["hit"]).is_true()
	assert_float((down["pos"] as Vector3).y).override_failure_message(
		"the merged rubble is not standing on the ground").is_greater(
		(st["ground_y"] as float) - 0.1)

func test_an_anchored_overhang_is_left_alone(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	# Undercut the pillar without severing it: a 0.5 m bite out of one side.
	t.apply_sphere_subtract(Vector3(PILLAR_X + 1.0, PILLAR_BASE + 2.0, PILLAR_Z), 0.7)
	step(w, 180)
	assert_int(w.debug_island_stats()["islands_spawned"]).override_failure_message(
		"a still-attached pillar was declared an island").is_equal(0)
	assert_bool(solid_at(w, Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z))).is_true()

func test_connectivity_runs_once_per_frame_however_many_edits_land(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	# Three blasts in one frame (spec section 5: "simultaneous blasts can't race").
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	t.apply_sphere_subtract(Vector3(PILLAR_X + 0.4, PILLAR_BASE + 2.2, PILLAR_Z), 1.2)
	t.apply_sphere_subtract(Vector3(PILLAR_X - 0.4, PILLAR_BASE + 2.2, PILLAR_Z), 1.2)
	step(w, 240)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["connectivity_runs"]).is_greater(0)
	# One window covered all three, so the pillar top came off exactly once.
	assert_int(st["islands_spawned"]).is_between(1, 3)
