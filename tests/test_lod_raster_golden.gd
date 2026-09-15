extends GdUnitTestSuite

# Characterization, not specification. These numbers are whatever the LoD raster's
# Bayer dissolve produced on 2026-09-15 at the seam camera (100, 68, 202); they exist
# so that "the fade did not change" is a measurement rather than an assertion. If an
# intentional redesign moves them, re-record them in the same commit that causes the
# move and say so in the message.
#
# Why this probe bites where test_lod_gbuffer.gd and test_lod_seam.gd do not: the
# dissolve ratio only matters for far-field pixels whose Euclidean distance falls
# inside the fade band (64-80 m here). The gbuffer suite's cameras sit beyond the band
# (the ratio saturates at 1 either way) and the seam suite's claimed/unclaimed counts
# are binary per pixel -- snapping the dissolve to a hard step still leaves every band
# pixel claimed by exactly one field, and depth hides the extra far fragments where
# both fields overlap. This probe renders the far field alone over a view with band
# pixels and pins how many it keeps (coverage) and their accumulated reverse-Z depth
# (depth_sum, which also sees swaps that keep the silhouette). Collapsing fade_end to
# fade_start keeps every in-band far pixel instead of a fraction t of them: coverage
# 0.097114 -> 0.102458, depth_sum 4.693420 -> 5.418561.
#
# Same world construction as tests/test_lod_gbuffer.gd: a local rendering device,
# physics off, the walk settled at the probe camera until its queues go quiet.

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
	w.stream_radius_m = 1300.0
	w.max_lod_pages = 32768
	w.near_field_scale = 1.0
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

const SETTLE_BUDGET := 6000
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d: Dictionary = w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# Three repeated runs on 2026-09-15 were bit-identical (spread 0), so these tolerances
# are fixed floors, not twice a measured spread: the break moves coverage by 2.7x TOL
# and depth_sum by 14x TOL. If this ever goes flaky, widen TOL before re-recording.
const GOLDEN := {"coverage": 0.097114, "depth_sum": 4.693420}
const TOL_COVERAGE := 0.002
const TOL_DEPTH_SUM := 0.05

func test_lod_fade_matches_the_recorded_golden(timeout := 180000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 68.0, 202.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var r: Dictionary = w.hooks().debug_lod_render_probe(pos, fwd, 256, 144)
	assert_float(r["coverage"]).override_failure_message(
		"coverage moved: golden %f, got %f" % [GOLDEN["coverage"], r["coverage"]]
	).is_equal_approx(GOLDEN["coverage"], TOL_COVERAGE)
	assert_float(r["depth_sum"]).override_failure_message(
		"depth_sum moved: golden %f, got %f" % [GOLDEN["depth_sum"], r["depth_sum"]]
	).is_equal_approx(GOLDEN["depth_sum"], TOL_DEPTH_SUM)
