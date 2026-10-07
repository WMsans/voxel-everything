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
	w.stream_radius_m = 1000.0 # small but viable: the walk descends only into a node
	# whose eight children are all in-radius (L6 spans 819 m); below ~940 m it stalls
	# at 2 roots, so 1000 is the floor for these cameras
	# 32768, the shipped default (LodSystem::max_lod_pages_): 16384 was enough only while
	# the walk stalled on out-of-radius siblings and drew coarse roots instead of the far
	# field; a complete cut for this camera needs ~19k pages.
	w.max_lod_pages = 32768
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
# "converged" rather than "between batches". Measured: ~350-400 ticks pre-trees, ~2519 with
# trees in the default pipeline (clean quiet streak, op_overflow=0), so the budget is margin.
const SETTLE_BUDGET := 3500
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

func test_an_edit_rebuilds_every_level_it_touches(timeout := 180000) -> void:
	var w := make_world()
	w.set_effect_enabled("near_field", false) # Build fine LoD levels around the nearby edit too.
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var before := w.hooks().debug_lod_render_probe(pos, fwd, 1280, 720)
	var surface := w.hooks().debug_raycast(Vector3(400.0, 180.0, 380.0), Vector3.DOWN)
	assert_bool(surface["hit"]).is_true()
	w.hooks().debug_apply_sphere_subtract(surface["pos"], 8.0)
	# The LoD mark is queued under the edit lock and applied by the next tick's drain
	# (core/edit_pipeline.h). Drain it here rather than ticking: a tick would clear the dirty
	# flags this case is about to read, because note_building clears them at submission.
	w.hooks().debug_drain_invalidations()
	var dirty := w.hooks().debug_lod_stats()
	# The probe runs one LoD tick, so it draws the post-edit walk; the stale-beats-missing
	# assertion below is vacuous if it reads the pre-edit draw list.
	var d := w.hooks().debug_lod_render_probe(pos, fwd, 1280, 720)
	assert_int(dirty["dirty_chunks"]).override_failure_message(
		"an 8 m crater dirtied no LoD chunks").is_greater(0)
	assert_int(dirty["dirty_levels"]).override_failure_message(
		"an 8 m crater dirtied %d levels, expected every level it reaches" % dirty["dirty_levels"]
		).is_greater_equal(4)
	# Stale beats missing: nothing is un-drawn while the rebuild is queued. Measured as the
	# far field's screen coverage, not its page count: the edit resets the empty chunks it
	# touches to unknown, so their parents draw in place of eight children until the rebuild
	# lands -- fewer pages, the same ground. Pages fell 15% that way at the 6 px target while
	# coverage rose (0.189 -> 0.194: the crater walls are new surface).
	assert_float(d["coverage"]).override_failure_message(
		"the far field un-drew ground while the rebuild was queued: coverage %f -> %f" % [
		before["coverage"], d["coverage"]]).is_greater_equal(before["coverage"] * 0.99)
	assert_bool(await settle(w, pos, fwd)).is_true()
	assert_int(w.hooks().debug_lod_stats()["dirty_chunks"]).override_failure_message(
		"the dirty chunks never finished rebuilding").is_equal(0)

func test_a_far_edit_is_visible_in_the_far_field(timeout := 180000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var before := w.hooks().debug_lod_render_probe(pos, fwd, 256, 144)
	w.hooks().debug_apply_sphere_subtract(Vector3(400.0, 55.0, 250.0), 20.0)
	assert_bool(await settle(w, pos, fwd)).is_true()
	var after := w.hooks().debug_lod_render_probe(pos, fwd, 256, 144)
	# A 20 m crater 150 m away must change what the far field draws. Measure the DEPTH image,
	# not the silhouette: the seam now sits where the near field's bricks actually stop
	# (ve::lod_fade_band), so the far field reaches much closer and a crater has more far
	# field behind it -- it swaps one surface for another instead of punching through to sky,
	# and total coverage barely moves (measured: 2.7e-5) while the depths plainly change.
	assert_float(absf(after["depth_sum"] - before["depth_sum"])).override_failure_message(
		"a 20 m crater at 150 m changed nothing in the far field").is_greater(0.0)

# Trees (Task 7): this case needs TWO full far-field settles (before and after the
# teardown/reinit), each measured at ~2519 ticks with trees in the default pipeline --
# the 40 s ceiling was tuned for two ~400-tick pre-trees settles. The siblings in this
# file already run under 180 s.
# Regression: the transparency toggle gates ONLY the shell half of a batch. With it off the
# far field must still stream -- the toggle empties the shell candidate set, so nothing in
# this convergence depends on a shell build. Under the outer-gate regression the walk's
# requests were never drained, requests_pending never reached zero and this settle fails.
func test_the_far_field_streams_with_transparency_off(timeout := 180000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	w.set_transparency_value("enabled", 0.0)
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d := w.hooks().debug_lod_stats()
	assert_int(d["draw_pages"]).override_failure_message(
		"the far field never streamed with transparency off").is_greater(0)
	assert_int(d["dirty_chunks"]).override_failure_message(
		"%d chunks never finished rebuilding with transparency off" % d["dirty_chunks"]
		).is_equal(0)

func test_teardown_and_reinit_leave_no_pages_behind(timeout := 180000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	assert_int(w.hooks().debug_lod_stats()["pages_used"]).is_greater(0)
	w.hooks().debug_teardown_atlas()
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var d := w.hooks().debug_lod_stats()
	assert_int(d["pages_used"]).override_failure_message(
		"%d pages survived a teardown" % d["pages_used"]).is_equal(0)
	assert_int(d["chunks_resident"]).is_equal(0)
	# And it still streams afterwards.
	assert_bool(await settle(w, pos, fwd)).is_true()
	assert_int(w.hooks().debug_lod_stats()["pages_used"]).is_greater(0)
