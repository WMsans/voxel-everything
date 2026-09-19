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
#
# min_ao/lit_luma re-recorded 2026-09-14: the probe now reads the SSAO the shipped
# headless frame produced (Task 6) instead of marching a fixed 200 m itself. Isolated
# with shipped knobs: setting near_field_scale = 1.0 restores every min_ao bit-exactly,
# so the darker extremes are the frame's 0.66 compositing, and grass max_blades = 0 /
# ssgi off move nothing. lit_luma rises on the two far-seeing cameras only (down_close
# is unchanged within tolerance) because far pixels are now LoD mesh past the fade band
# instead of field raymarched to 200 m. ran stays true and max_ao stays 1.0 throughout.
#
# min_ao/lit_luma re-recorded 2026-09-18: near-field grass covers the whole resident sphere
# now, not a 10 m slab around the camera, and all three cameras stand 14 m above the ground --
# so where the note above could say "grass max_blades = 0 moves nothing", these probes now see
# blades. Isolated the same way: setting grass enabled = 0 returns every one of these six
# numbers to the line above, min_ao bit-exactly. min_ao is 0 because AO between blades is
# total, and lit_luma rises by about 0.01 on every camera because grass is brighter than the
# ground it covers. max_ao stays 1.0 and ran stays true.
# min_ao/lit_luma re-recorded 2026-09-19: the leaf raster draws cards in the shipped frame
# (Task 12), and the horizon camera's frame gains canopy pixels -- lit_luma 0.355182 ->
# 0.344913 there (leaves shade darker than the sky/terrain they cover at that distance).
# down_close and oblique are unchanged; min_ao/max_ao unchanged; ran stays true. Same
# re-record policy as the frame golden above.
# lit_luma re-recorded 2026-09-19: canopy sway on the shared gust field (Task 13) moves the
# cards in these probes' shipped frames -- oblique 0.318546 -> 0.315139 (the largest move,
# inside TOL_LUMA but most of its budget), horizon 0.344913 -> 0.344768, down_close stays at
# 0.252933 to the recorded precision (no canopy pixels). min_ao/max_ao unchanged; ran stays
# true. Same policy again: intentional change, recorded in the commit that causes it.
# lit_luma re-recorded 2026-09-19 (final review wave): the spec §4 height band took the
# out-of-band canopies out of the horizon frame -- the dark card pixels that Task 12's note
# saw shade away, and lit_luma rises back: 0.344768 -> 0.354178 (+0.0094, over TOL_LUMA).
# down_close and oblique stayed inside tolerance; min_ao/max_ao unchanged; ran stays true.
# Same policy: intentional change, re-recorded in the commit that causes it.
const GOLDEN := {
	"down_close": {"min_ao": 0.000000, "max_ao": 1.000000, "lit_luma": 0.252934},
	"oblique": {"min_ao": 0.000000, "max_ao": 1.000000, "lit_luma": 0.315139},
	"horizon": {"min_ao": 0.000000, "max_ao": 1.000000, "lit_luma": 0.354178},
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
