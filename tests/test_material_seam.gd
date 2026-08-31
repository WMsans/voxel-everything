extends GdUnitTestSuite

# The artifact this file exists to prevent: per-point hardness makes the carve field
# overestimate free space beside a hard-material seam, and the near-field marcher --
# t += max(d * 0.9, 0.005) -- steps straight through the barely-carved lip. The Eikonal
# clamp on the brick lattice (ve::clamp_brick_lattice) is what stops it. If someone removes
# or mis-gates the clamp, this is the test that goes red.

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

# Two hardnesses meeting inside one crater is the exact geometry that distorts the field.
func build_seam(w: VoxelWorld) -> Vector3:
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
	# One carve spanning both: it eats deep into the soft side and barely into the hard.
	w.hooks().debug_apply_sphere_subtract(centre, 3.0)
	settle(w)
	return centre

# The marcher and the analytic raycast walk the same world by different means. A ray that
# leaks through the seam lip reports a hit far behind where the field says the surface is.
func test_rays_do_not_leak_through_the_seam_lip() -> void:
	var w := make_world()
	var centre := build_seam(w)
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
		"%d of %d rays across the hardness seam missed a surface the field reports: the Eikonal clamp is not repairing the carve field" % [leaks, tested]).is_equal(0)

# The other half: hardness must actually be doing something, or the test above passes
# trivially on a world where nothing distorted the field in the first place.
func test_the_seam_carve_is_actually_asymmetric() -> void:
	var w := make_world()
	var centre := build_seam(w)
	# 1.2 m either side of the carve centre: inside the soft crater, outside the hard one.
	var soft_state := w.hooks().debug_cell_state(Vector3i(
		int((centre.x + 1.2) / 0.8), int(centre.y / 0.8), int(centre.z / 0.8)))
	var hard_state := w.hooks().debug_cell_state(Vector3i(
		int((centre.x - 1.2) / 0.8), int(centre.y / 0.8), int(centre.z / 0.8)))
	assert_int(soft_state).override_failure_message(
		"the soft side of the seam did not carve: hardness is not being applied"
		).is_not_equal(hard_state)

func terrain_height(x: float, z: float) -> float:
	return 51.2 + 6.0 * sin(x * 0.11) * cos(z * 0.13) \
		+ 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043) \
		+ sin(x * 0.23 + z * 0.19)

# A hard surface already forms a hardness seam with air: air uses the baseline 1.0 while
# the solid uses its material hardness. This is the ordinary "break a rock" case, without
# a second solid material in the same brick to accidentally switch the repair on.
func test_rays_do_not_leak_through_a_hard_surface_beside_air() -> void:
	var w := make_world()
	var centre := Vector3(20.0, 49.56, 20.0)
	w.hooks().debug_apply_sphere_paint(centre, 5.0, 2) # rock, hardness 3.0
	settle(w)
	w.hooks().debug_apply_sphere_subtract(centre, 3.0)
	settle(w)

	var leaks := 0
	# The hard material only carves to radius 1.0. Rays in this annulus should hit the
	# untouched procedural surface; a lattice left discontinuous at air steps through it.
	# Use that closed-form surface as the oracle: the CPU raycast is also a sphere tracer and
	# reproduces this bug, so comparing one leaky marcher to another would hide the artifact.
	for i in range(24):
		var angle := TAU * float(i) / 24.0
		var origin := centre + Vector3(cos(angle) * 1.8, 8.0, sin(angle) * 1.8)
		var marched: Dictionary = w.hooks().debug_raymarch_gbuffer(origin, Vector3(0, -1, 0))
		var expected_y := terrain_height(origin.x, origin.z)
		if not marched["hit"] or absf(float(marched["position"].y) - expected_y) > 0.15:
			leaks += 1
	assert_int(leaks).override_failure_message(
		"%d of 24 rays leaked through a hard-material surface beside air" % leaks
		).is_equal(0)
