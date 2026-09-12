extends GdUnitTestSuite

# Characterization, not specification. These numbers are whatever the hooked grass raster
# produced on 2026-09-12 with the Task 8 shading (wind, root-to-tip gradient, clumping,
# flowers, bayer distance fade); they exist so that "the image did not change" is a
# measurement rather than an assertion. If an intentional redesign moves them, re-record
# them in the same commit that causes the move and say so in the message.
#
# Wind time needs no pinning code: a fresh local-device world has beauty_frame() == 0
# (orchestrator.h initializes it to 0 and only the compositor increments it, which never
# runs on local-device worlds), so the hooked scatter/raster drive runs with
# time == 0.0 / 60.0 every run -- the same expression the compositor passes in production.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction every other GPU suite in this repo uses (see tests/test_grass.gd):
# a local rendering device, physics off, streamed until the chunk queue goes quiet. The
# hooked drive looks straight down from the last streamed centre over grass terrain.
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

# Recorded from a clean build on 2026-09-12 (74 blades / 222 vertices in the hooked
# 64x64 top-down view; min is the cleared-black background, max a sunlit blade pixel,
# mean the sparse top-down coverage). Tolerances mirror the SSAO golden's tightness:
# these are deterministic GPU results from a deterministic scene at a pinned wind time,
# not sampled statistics. -1.0 on any key means the hooked drive did not measure
# (never on local-device worlds; see debug_grass_stats).
const GOLDEN := {"min_luma": 0.000000, "max_luma": 0.532495, "mean_luma": 0.000229}
const TOL_EXTREME := 0.002
const TOL_MEAN := 0.004

func test_grass_shading_matches_the_recorded_golden() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["ran"]).override_failure_message(
		"Grass scatter did not run").is_true()
	assert_bool(d["drawn"]).override_failure_message(
		"Grass raster did not draw").is_true()
	assert_float(d["min_luma"]).override_failure_message(
		"min_luma moved: golden %f, got %f" % [GOLDEN["min_luma"], d["min_luma"]]
	).is_equal_approx(GOLDEN["min_luma"], TOL_EXTREME)
	assert_float(d["max_luma"]).override_failure_message(
		"max_luma moved: golden %f, got %f" % [GOLDEN["max_luma"], d["max_luma"]]
	).is_equal_approx(GOLDEN["max_luma"], TOL_EXTREME)
	assert_float(d["mean_luma"]).override_failure_message(
		"mean_luma moved: golden %f, got %f" % [GOLDEN["mean_luma"], d["mean_luma"]]
	).is_equal_approx(GOLDEN["mean_luma"], TOL_MEAN)
