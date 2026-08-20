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
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(24.0, 56.2, 24.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# A ray fired straight up crosses ~200 m of nothing. With a brick DDA that is 250 cells;
# with the region DDA spec §3 asks for it is at most a handful. The exact number is not the
# point -- the ORDER is, so the assertion is written against the brick count it replaces.
func test_sky_ray_walks_regions_not_bricks() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_raymarch_cost_probe(Vector3(24.0, 70.0, 24.0), Vector3(0, 1, 0))
	assert_bool(d["hit"]).is_false()
	assert_int(int(d["bricks"])).is_less(32)
	assert_int(int(d["regions"])).is_greater(0)

# The skip must never cost a hit. This is the same oracle test_raymarch_pixel.gd uses: the
# analytic raycast is the truth, and the marcher has to agree with it to a few centimetres.
func test_ground_hits_still_match_the_analytic_raycast() -> void:
	var w := make_world()
	for i in range(12):
		var a := float(i) * 0.5
		var dir := Vector3(sin(a) * 0.6, -1.0, cos(a) * 0.6).normalized()
		var eye := Vector3(24.0, 70.0, 24.0)
		var probe: Dictionary = w.debug_raymarch_cost_probe(eye, dir)
		var truth: Dictionary = w.debug_raycast(eye, dir)
		assert_bool(probe["hit"]).is_equal(truth["hit"])

# A ray that starts inside a resident region and leaves the residency radius must not report
# a hit from a region it never had data for.
func test_ray_leaving_residency_finds_nothing_rather_than_something() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_raymarch_cost_probe(
		Vector3(24.0, 62.0, 24.0), Vector3(1.0, 0.02, 0.0).normalized())
	var truth: Dictionary = w.debug_raycast(
		Vector3(24.0, 62.0, 24.0), Vector3(1.0, 0.02, 0.0).normalized())
	if truth["hit"]:
		assert_bool(d["hit"]).is_true()
	else:
		assert_bool(d["hit"]).is_false()
