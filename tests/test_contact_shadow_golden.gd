extends GdUnitTestSuite

# Characterization, not specification. These numbers are whatever the hooked
# contact shadow apply produced on 2026-09-15 on the crater fixture; they exist
# so that "the image did not change" is a measurement rather than an assertion.
# If an intentional redesign moves them, re-record them in the same commit that
# causes the move and say so in the message.
#
# The probe runs the shipped frame with SSR and outlines disabled, so
# mean_darkening isolates the contact apply: scene colour before the post-opaque
# stages minus scene colour after, averaged over the frame. Zeroing the apply
# strength (params.y in contact_shadow_pass.cpp) drives it to exactly 0, which
# is what this golden catches: no other contact_shadow test observes the apply
# output on its own (the mask tests read the mask texture; the darkening tests
# run with SSR and outlines re-enabled, whose own darkening masks the break).
#
# Recorded 2026-09-15: three repeated runs on the unmodified commit agree to
# the printed precision (mean_darkening=0.006335, spread 0), so the tolerance
# is a fixed small floor: the break this guards (apply strength zeroed) drives
# the value to exactly 0, 6x the tolerance away.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction the other contact shadow tests use (see
# tests/test_contact_shadow.gd): a local rendering device, physics off, the
# golden pipeline pinning the crater fixture, streamed until quiet.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

const GOLDEN := {"mean_darkening": 0.006335}
const TOL := 0.001

func test_contact_apply_matches_the_recorded_golden() -> void:
	var w := make_world()
	w.set_effect_enabled("contact_shadows", true)
	w.set_effect_enabled("ssr", false)
	w.set_effect_enabled("outlines", false)
	# Side view into the crater: shadowed pixels fill the frame (mask_mean ~0.85),
	# so the apply's darkening is a measurable signal, not noise.
	var views := [
		[Vector3(30.0, 52.0, 22.0), Vector3(0.0, -0.36, 0.93).normalized()],
	]
	for v in views:
		var d: Dictionary = w.hooks().debug_contact_shadow_probe(v[0], v[1], 128, 128)
		assert_float(d["mean_darkening"]).override_failure_message(
			"mean_darkening moved: golden %f, got %f" % [GOLDEN["mean_darkening"], d["mean_darkening"]]
		).is_equal_approx(GOLDEN["mean_darkening"], TOL)
