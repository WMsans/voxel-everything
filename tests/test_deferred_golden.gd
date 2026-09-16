extends GdUnitTestSuite

# Characterization, not specification. These numbers are whatever the shipped
# lit frame produced on 2026-09-15 over open ground; they exist so that "the
# image did not change" is a measurement rather than an assertion. If an
# intentional redesign moves them, re-record them in the same commit that
# causes the move and say so in the message.
#
# probe_mode 0 is the full lit path: the deferred flags word (SSAO multiply,
# SSGI ambient, sun-map shadows) is applied. Zeroing the flags word lifts all
# three at once, which moves mean_luma by far more than run-to-run noise --
# and no other deferred test observes the flagged lit output (the cel tests
# call the shader function directly; the position test uses probe_mode 2; the
# sky test looks up; the sun-shadow test only asserts a direction).
#
# Recorded 2026-09-15: three repeated runs agree to the printed precision
# (spread 0), so the tolerance is a fixed small floor.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction the other deferred tests use (see
# tests/test_deferred.gd): a local rendering device, physics off, streamed
# until quiet.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

const GOLDEN := {"mean_luma": 0.033779}
const TOL := 0.002

func test_lit_frame_matches_the_recorded_golden() -> void:
	var w := make_world()
	# Top-down over the crater: shadowed walls fill the frame, so the flags the
	# break zeroes actually shape the image (open-ground views move 3x less).
	var d: Dictionary = w.hooks().debug_deferred_probe(
		Vector3(30.0, 56.0, 30.0), Vector3(0.0, -1.0, 0.0), 192, 144, 0)
	assert_float(d["mean_luma"]).override_failure_message(
		"mean_luma moved: golden %f, got %f" % [GOLDEN["mean_luma"], d["mean_luma"]]
	).is_equal_approx(GOLDEN["mean_luma"], TOL)
