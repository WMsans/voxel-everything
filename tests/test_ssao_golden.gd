extends GdUnitTestSuite

# Characterization, not specification. These numbers are whatever the pass produced on
# 2026-09-08 before the W1 optimisations; they exist so that "the image did not change"
# is a measurement rather than an assertion. If an intentional redesign moves them
# (Task 6's half-res chain would), re-record them in the same commit that causes the
# move and say so in the message.

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
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# Three cameras chosen to cover the cases the optimisations touch:
#   down_close  - near ground, large projected radius, the early-out must NOT fire
#   oblique     - mixed near and far, the early-out fires on part of the frame
#   horizon     - mostly distant terrain, the early-out fires almost everywhere
const CAMERAS := {
	"down_close": [Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2)],
	"oblique": [Vector3(30.0, 70.0, 30.0), Vector3(0.5, -0.5, 0.5)],
	"horizon": [Vector3(30.0, 70.0, 30.0), Vector3(0.35, -0.2, 0.35)],
}

func _probe(w: VoxelWorld, key: String) -> Dictionary:
	var c: Array = CAMERAS[key]
	return w.hooks().debug_ssao_probe(c[0], (c[1] as Vector3).normalized(), 128, 128)

# Recorded from a clean build at spec time. Tolerances are tight on purpose: these are
# deterministic GPU results from a deterministic analytic field, not sampled statistics.
#
# min_ao re-recorded 2026-09-09 for the half-res AO target (Task 6's chain, now landed).
# Only min_ao moved: it is the single darkest texel in the frame, and at half the linear
# resolution each texel covers four times the area, so the extreme is averaged away.
# max_ao and lit_luma did NOT move outside tolerance — the lit image's mean luminance is
# unchanged, which is the number that says the picture still looks the same.
const GOLDEN := {
	"down_close": {"min_ao": 0.745098, "max_ao": 1.000000, "lit_luma": 0.241331},
	"oblique": {"min_ao": 0.792157, "max_ao": 1.000000, "lit_luma": 0.297197},
	"horizon": {"min_ao": 0.792157, "max_ao": 1.000000, "lit_luma": 0.329064},
}
const TOL_AO := 0.002
const TOL_LUMA := 0.004

func test_ssao_statistics_match_the_recorded_golden() -> void:
	var w := make_world()
	for key in CAMERAS.keys():
		var d: Dictionary = _probe(w, key)
		assert_bool(d["ran"]).override_failure_message(
			"SSAO did not run for camera '%s'" % key).is_true()
		var g: Dictionary = GOLDEN[key]
		assert_float(d["min_ao"]).override_failure_message(
			"min_ao moved for '%s': golden %f, got %f" % [key, g["min_ao"], d["min_ao"]]
		).is_equal_approx(g["min_ao"], TOL_AO)
		assert_float(d["max_ao"]).override_failure_message(
			"max_ao moved for '%s': golden %f, got %f" % [key, g["max_ao"], d["max_ao"]]
		).is_equal_approx(g["max_ao"], TOL_AO)
		assert_float(d["lit_luma"]).override_failure_message(
			"lit_luma moved for '%s': golden %f, got %f" % [key, g["lit_luma"], d["lit_luma"]]
		).is_equal_approx(g["lit_luma"], TOL_LUMA)
