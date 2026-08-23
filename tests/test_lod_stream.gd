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
	w.world_size_regions = Vector3i(16, 5, 16)
	w.max_lod_pages = 16384
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# The walk descends only into a node whose eight children are all resident, so the far field
# converges over HUNDREDS of ticks and at a rate set by build throughput, not by frame count.
# Wait on the condition (errata 6): fixed frame counts were tuned to the old occluded
# 590 ms vsync frame and settle nothing on the current runner, which keeps vsync enabled
# intentionally (gdunit_tests.sh) at a normal display rate.
# requests_pending comes from the walk that ran BEFORE this tick collected its results, so it
# dips to zero for a tick or two while a batch lands -- the streak is what makes it mean
# "converged" rather than "between batches". Measured: ~350-400 ticks, so the budget is margin.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

func test_an_edit_rebuilds_every_level_it_touches(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var before := w.hooks().debug_lod_stats()
	w.hooks().debug_apply_sphere_subtract(Vector3(380.0, 55.0, 250.0), 8.0)
	# Run one LoD tick so lod_walk_ reflects the post-edit walk; the stale-beats-missing
	# assertion below is vacuous if it reads the pre-edit draw list.
	w.hooks().debug_lod_tick(pos, fwd)
	var d := w.hooks().debug_lod_stats()
	assert_int(d["dirty_chunks"]).override_failure_message(
		"an 8 m crater dirtied no LoD chunks").is_greater(0)
	assert_int(d["dirty_levels"]).override_failure_message(
		"an 8 m crater dirtied %d levels, expected every level it reaches" % d["dirty_levels"]
		).is_greater_equal(4)
	# Stale beats missing: nothing is un-drawn while the rebuild is queued.
	assert_int(d["draw_pages"]).is_greater_equal(before["draw_pages"] * 0.9)
	await settle(w, pos, fwd)
	assert_int(w.hooks().debug_lod_stats()["dirty_chunks"]).override_failure_message(
		"the dirty chunks never finished rebuilding").is_equal(0)

func test_a_far_edit_is_visible_in_the_far_field(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var before := w.hooks().debug_lod_render_probe(pos, fwd, 256, 144)
	w.hooks().debug_apply_sphere_subtract(Vector3(400.0, 55.0, 250.0), 20.0)
	await settle(w, pos, fwd)
	var after := w.hooks().debug_lod_render_probe(pos, fwd, 256, 144)
	# A 20 m crater 150 m away must change what the far field draws. Measure the DEPTH image,
	# not the silhouette: the seam now sits where the near field's bricks actually stop
	# (ve::lod_fade_band), so the far field reaches much closer and a crater has more far
	# field behind it -- it swaps one surface for another instead of punching through to sky,
	# and total coverage barely moves (measured: 2.7e-5) while the depths plainly change.
	assert_float(absf(after["depth_sum"] - before["depth_sum"])).override_failure_message(
		"a 20 m crater at 150 m changed nothing in the far field").is_greater(0.0)

func test_teardown_and_reinit_leave_no_pages_behind(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	assert_int(w.hooks().debug_lod_stats()["pages_used"]).is_greater(0)
	w.hooks().debug_teardown_atlas()
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var d := w.hooks().debug_lod_stats()
	assert_int(d["pages_used"]).override_failure_message(
		"%d pages survived a teardown" % d["pages_used"]).is_equal(0)
	assert_int(d["chunks_resident"]).is_equal(0)
	# And it still streams afterwards.
	await settle(w, pos, fwd)
	assert_int(w.hooks().debug_lod_stats()["pages_used"]).is_greater(0)
