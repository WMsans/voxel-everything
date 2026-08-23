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
	w.ensure_initialized()
	settle(w, Vector3(24.0, 56.0, 24.0))
	return w

# Settled means several consecutive quiet frames: the streamer paces stream-in against an
# atlas free count it reads back a frame or more behind the GPU, so a single frame with
# nothing to do is that pause, not the end of the work (six > its in-flight window).
func settle(w: VoxelWorld, cam: Vector3, frames := 120) -> void:
	var quiet := 0
	for i in range(frames):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(cam) == 0 else 0
		if quiet >= 6:
			return

# The cost probe is the instrument Tasks 3 and 4 are measured with, so it has to mean
# something before they run: a ray fired at the ground must cost more than one fired at the
# sky, and neither may report zero work when the marcher ran.
func test_ground_ray_costs_more_than_sky_ray() -> void:
	var w := make_world()
	var eye := Vector3(24.0, 62.0, 24.0)
	var down: Dictionary = w.hooks().debug_raymarch_cost_probe(eye, Vector3(0.2, -1.0, 0.2).normalized())
	var up: Dictionary = w.hooks().debug_raymarch_cost_probe(eye, Vector3(0.0, 1.0, 0.0))
	assert_bool(down["hit"]).is_true()
	assert_bool(up["hit"]).is_false()
	assert_int(down["steps"]).is_greater(0)
	assert_int(down["steps"]).is_greater(int(up["steps"]))

func test_probe_reports_brick_cells_visited() -> void:
	var w := make_world()
	# A near-horizontal ray crosses many brick cells before it finds anything; that count is
	# what the region DDA in Task 4 is supposed to collapse.
	var d: Dictionary = w.hooks().debug_raymarch_cost_probe(
		Vector3(24.0, 70.0, 24.0), Vector3(1.0, -0.02, 0.0).normalized())
	assert_int(d["bricks"]).is_greater(20)
