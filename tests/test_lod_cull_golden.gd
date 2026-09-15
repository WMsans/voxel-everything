extends GdUnitTestSuite

# Characterization, not specification. This number is whatever the production LoD
# occlusion cull reported on 2026-09-15 for a yawed horizon view over open terrain;
# it exists so that "the cull did not change" is a measurement rather than an
# assertion. If an intentional redesign moves it, re-record it in the same commit that
# causes the move and say so in the message.
#
# Why this probe bites where test_lod_cull.gd does not: that suite drives
# debug_lod_cull_probe, which rebuilds a synthetic everything-far HiZ pyramid before
# every run and ticks the page list for the probe view, so the cull can only ever
# exercise its frustum path on an already-frustum-filtered list -- zeroing the pushed
# page count changes nothing observable there (ratio 0.0, drawn == before, either
# way). This probe instead runs shipped headless frames, which feed the cull the real
# HiZ pyramid and expose its shipped stats counter through debug_lod_stats. The yaw
# puts a crescent of fresh pages through the cull with real occluders behind them;
# on open terrain nothing is occluded, so the shipped ratio reads 0.0. Zeroing the
# page count keeps the whole crescent while reporting zero drawn, which drives the
# same counter to 0.333 -- the bite is inverted (the break raises the ratio), and the
# comment records that so a future reader does not "fix" the test backwards.
#
# The walk settles are best-effort, not asserted: the ratio stabilizes on the macro
# page structure well before the walk goes fully quiet, and asserting quiet would fail
# the test on machines with slower build throughput while measuring the same cull.
# What IS asserted is that every frame ran.

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
	w.stream_radius_m = 1000.0
	w.max_lod_pages = 8192
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> void:
	var quiet := 0
	for i in range(2500):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var s := w.hooks().debug_lod_stats()
		quiet = quiet + 1 if s["requests_pending"] == 0 and s["builds_in_flight"] == 0 else 0
		if quiet >= 8:
			return

func drive(w: VoxelWorld, pos: Vector3, fwd: Vector3, n: int) -> bool:
	var ran := false
	for i in range(n):
		var s := w.hooks().debug_ssao_probe(pos, fwd, 320, 180)
		ran = ran or s["ran"]
	return ran

# Three repeated runs on 2026-09-15 read exactly 0.0 (spread 0), so this tolerance is
# a fixed floor, not twice a measured spread: the break drives the ratio to 0.333,
# nearly 7x TOL above golden. If this ever goes flaky, widen TOL before re-recording.
const GOLDEN := 0.0
const TOL := 0.05

func test_production_cull_ratio_matches_the_recorded_golden(timeout := 500000) -> void:
	var w := make_world()
	var pos := Vector3(30.0, 70.0, 30.0)
	var fa := Vector3(0.35, -0.2, 0.35).normalized()
	var fb := Vector3(0.85, -0.2, 0.35).normalized()
	await settle(w, pos, fa)
	assert_bool(await drive(w, pos, fa, 2)).override_failure_message(
		"no headless frame ran").is_true()
	await settle(w, pos, fb)
	assert_bool(await drive(w, pos, fb, 4)).override_failure_message(
		"no headless frame ran").is_true()
	var st := w.hooks().debug_lod_stats()
	assert_float(st["culled_ratio"]).override_failure_message(
		"culled_ratio moved: golden %f, got %f" % [GOLDEN, st["culled_ratio"]]
	).is_equal_approx(GOLDEN, TOL)
