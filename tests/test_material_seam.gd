extends GdUnitTestSuite

# A removal operation must remain one geometric shape across a material seam. Hardness is
# selected once from the center ray's material and resolved into the stored radius before
# the field evaluator sees the op; per-point material lookups would make this suite fail.

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
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func settle(w: VoxelWorld) -> void:
	var quiet := 0
	for i in range(200):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break

# Two hardnesses meeting inside one crater is the regression geometry.
func build_seam(w: VoxelWorld) -> Dictionary:
	var centre := Vector3(20.0, 47.5, 20.0)
	# A slab of the hardest material, then a slab of the softest, sharing a plane.
	var hard := 0
	var soft := 0
	var hard_h := 0.0
	var soft_h := 999.0
	for m in w.material_table():
		var h := float(m["hardness"])
		if h > hard_h:
			hard_h = h
			hard = int(m["id"])
		if h < soft_h:
			soft_h = h
			soft = int(m["id"])
	assert_float(hard_h).override_failure_message(
		"every material has the same hardness: this test cannot build a seam"
		).is_greater(soft_h)
	w.hooks().debug_apply_sphere_paint(centre - Vector3(2.0, 0, 0), 2.0, hard)
	w.hooks().debug_apply_sphere_paint(centre + Vector3(2.0, 0, 0), 2.0, soft)
	settle(w)
	# Resolve the selected center material once. Both painted sides must then consume this
	# same effective radius, regardless of their own material IDs.
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	tool.apply_sphere_subtract(centre, 3.0, hard)
	settle(w)
	return {"centre": centre, "effective_radius": 3.0 / hard_h}

# The marcher and the analytic raycast walk the same world by different means. A ray that
# leaks through the seam lip reports a hit far behind where the field says the surface is.
func test_rays_do_not_leak_through_the_seam_lip() -> void:
	var w := make_world()
	var seam := build_seam(w)
	var centre: Vector3 = seam["centre"]
	var leaks := 0
	var tested := 0
	for i in range(24):
		var t := float(i) / 24.0
		# Sweep across the seam plane, aiming down into the crater from above.
		var origin := centre + Vector3(lerpf(-3.0, 3.0, t), 8.0, 0.0)
		var dir := Vector3(0, -1, 0)
		var hit: Dictionary = w.hooks().debug_raycast(origin, dir)
		if not hit["hit"]:
			continue
		tested += 1
		var marched: Color = w.hooks().debug_raymarch_pixel(origin, dir)
		if marched.a <= 0.0:
			leaks += 1 # the marcher saw sky where the field says there is surface
	assert_int(tested).override_failure_message(
		"no ray in the sweep hit the terrain; the fixture did not build").is_greater(8)
	assert_int(leaks).override_failure_message(
		"%d of %d rays across the material seam missed a surface the analytic field reports" % [leaks, tested]).is_equal(0)

func test_the_seam_carve_uses_one_effective_radius() -> void:
	var w := make_world()
	var seam := build_seam(w)
	var centre: Vector3 = seam["centre"]
	var effective_radius: float = seam["effective_radius"]
	var inside_offset := effective_radius * 0.5
	var outside_offset := effective_radius + 0.2
	var hard_inside: float = w.hooks().debug_field_sdf(
		centre - Vector3(inside_offset, 0, 0))
	var soft_inside: float = w.hooks().debug_field_sdf(
		centre + Vector3(inside_offset, 0, 0))
	var hard_outside: float = w.hooks().debug_field_sdf(
		centre - Vector3(outside_offset, 0, 0))
	var soft_outside: float = w.hooks().debug_field_sdf(
		centre + Vector3(outside_offset, 0, 0))
	assert_float(hard_inside).is_equal_approx(soft_inside, 0.0001)
	assert_float(hard_outside).is_equal_approx(soft_outside, 0.0001)
	assert_float(hard_inside).is_greater(0.0)
	assert_float(hard_outside).is_less(0.0)
