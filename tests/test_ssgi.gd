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
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_result_is_half_resolution() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 1)
	assert_int(d["width"]).is_equal(64)
	assert_int(d["height"]).is_equal(64)

# The first frame has no history to bounce from, so it must produce zero rather than reading
# an uninitialised texture and painting the screen with whatever was in memory.
func test_the_first_frame_bounces_nothing() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 1)
	assert_float(d["max_channel"]).is_equal_approx(0.0, 0.001)

# ...and once there IS a history, light bounces. The crater at (30, ~49, 30) is a bowl: its
# walls see each other, which is the case one-bounce GI exists to brighten.
func test_light_bounces_once_the_history_exists() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
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
	var a: Dictionary = w.debug_ssgi_probe(pos, fwd, 128, 128, 8)
	var b: Dictionary = w.debug_ssgi_probe(pos, fwd, 128, 128, 24)
	# A static camera over a static world: three times the frames must not mean three times
	# the light. Allow a wide band; the point is that it is bounded, not that it is equal.
	assert_float(float(b["mean_luma"])).is_less(float(a["mean_luma"]) * 2.0 + 0.01)

func test_turning_ssgi_off_produces_nothing_and_costs_no_dispatch() -> void:
	var w := make_world()
	w.set_effect_enabled("ssgi", false)
	var d: Dictionary = w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 8)
	assert_bool(d["ran"]).is_false()

# Keep the same previous lit image and current camera, but change only the matrix supplied
# as the previous-frame mapping. A motion-correct history sample must respond to that
# non-identity transform; sampling history at current-frame horizon UVs produces no change.
func test_temporal_history_uses_previous_camera_mapping() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_ssgi_reprojection_probe(
		Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		Vector3(34.0, 70.0, 30.0), Vector3(-0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_bool(d["non_identity"]).is_true()
	assert_float(float(d["mapping_delta"])).override_failure_message(
		"temporal lit history did not respond to previous-frame camera mapping").is_greater(0.0001)
