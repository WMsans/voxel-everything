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
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_result_is_half_resolution() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 1)
	assert_int(d["width"]).is_equal(64)
	assert_int(d["height"]).is_equal(64)

# The first frame has no history to bounce from, so it must produce zero rather than reading
# an uninitialised texture and painting the screen with whatever was in memory.
func test_the_first_frame_bounces_nothing() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 1)
	assert_float(d["max_channel"]).is_equal_approx(0.0, 0.001)

# ...and once there IS a history, light bounces. The crater at (30, ~49, 30) is a bowl: its
# walls see each other, which is the case one-bounce GI exists to brighten.
func test_light_bounces_once_the_history_exists() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 8)
	assert_float(d["max_channel"]).override_failure_message(
		"eight frames of history produced no bounce at all").is_greater(0.005)
	# ...and it did not blow up: temporal accumulation without a clamp diverges, and this is
	# the assertion that catches it.
	assert_float(d["max_channel"]).is_less(2.0)

func test_the_accumulation_converges_rather_than_climbing() -> void:
	var w := make_world()
	var pos := Vector3(30.0, 70.0, 30.0)
	var fwd := Vector3(0.2, -1.0, 0.2).normalized()
	var a: Dictionary = w.hooks().debug_ssgi_probe(pos, fwd, 128, 128, 8)
	var b: Dictionary = w.hooks().debug_ssgi_probe(pos, fwd, 128, 128, 24)
	# A static camera over a static world: three times the frames must not mean three times
	# the light. Allow a wide band; the point is that it is bounded, not that it is equal.
	assert_float(float(b["mean_luma"])).is_less(float(a["mean_luma"]) * 2.0 + 0.01)

func test_turning_ssgi_off_produces_nothing_and_costs_no_dispatch() -> void:
	var w := make_world()
	w.set_effect_enabled("ssgi", false)
	var d: Dictionary = w.hooks().debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 8)
	assert_bool(d["ran"]).is_false()

# The probe renders frame 1 at the previous camera and frame 2 at the current one, so
# frame 2 gathers the history through the view-projection the frame recorded at the end of
# frame 1. That gather reproduces the hand-fed reference (2.29e-4, 2026-09-14) to 2%, which
# is what ties it to the PREVIOUS mapping: a dead history would read ~0, gathering through
# the current matrix would read current_mapping_luma (~3.09e-4), and an ignored camera move
# would read the same-camera value (~1.05e-4). Delta isolation is not claimed: bounce growth
# and the running beauty_frame advance jointly between the two reads, so same-camera drift
# (9.49e-5) exceeds the moved delta (8.5e-5) at these levels (2026-09-14 analysis).
func test_temporal_history_uses_previous_camera_mapping() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssgi_reprojection_probe(
		Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		Vector3(34.0, 70.0, 30.0), Vector3(-0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_bool(d["non_identity"]).is_true()
	assert_float(float(d["mapping_luma"])).override_failure_message(
		"frame-2 gather did not come through the recorded previous-camera mapping").is_equal_approx(
		0.000229, 0.00004)

# ...and the same must hold after the G-buffer is thrown away and rebuilt. A runtime render
# scale change reconfigures the viewport, the engine drops the voxel_gbuf context, and
# GBuffer::ensure() recreates `history` with undefined contents. The latch that says "a
# history exists" describes the OLD texture, so it has to fall with it -- otherwise SSGI
# bounces uninitialised memory into a temporal accumulator that spreads it a texel per frame.
func test_the_history_latch_falls_when_the_gbuffer_is_reallocated() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssgi_history_latch_probe(128, 128, 96, 96)
	assert_bool(d["ran"]).override_failure_message("probe did not run").is_true()
	assert_bool(d["after_write"]).override_failure_message(
		"writing a history did not set the latch, so the test proves nothing").is_true()
	assert_bool(d["reallocated"]).override_failure_message(
		"the second ensure() did not reallocate, so the test proves nothing").is_true()
	assert_bool(d["after_realloc"]).override_failure_message(
		"the latch survived the reallocation: SSGI will sample an uninitialised history"
	).is_false()
