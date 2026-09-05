extends GdUnitTestSuite

# The artifact this file exists to prevent: a removal whose radius varies with the material
# AT EACH SAMPLE POINT. That kept the field's sign right but destroyed its magnitude as a
# distance bound beside a hard seam, and the near-field marcher -- t += max(d * 0.9, 0.005)
# -- stepped straight through the barely-carved lip.
#
# The fix is upstream of the field: ve::removal_radius resolves the CENTRE RAY's material
# once, when the op is built, and every evaluator then sees one ordinary sphere. So the
# invariant these tests hold is "one op, one radius, on both sides of a seam" -- if someone
# reintroduces a per-sample hardness lookup in ve::apply_op or shaders/field.glslh, the
# symmetry test goes red and the leak test follows it.

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

func make_tool(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

# The hardest and softest ids in the table, so the fixture keeps working when the table does.
func hardest(w: VoxelWorld) -> Dictionary:
	var best: Dictionary = w.material_table()[0]
	for m in w.material_table():
		if float(m["hardness"]) > float(best["hardness"]):
			best = m
	return best

func softest(w: VoxelWorld) -> Dictionary:
	var best: Dictionary = w.material_table()[0]
	for m in w.material_table():
		if float(m["hardness"]) < float(best["hardness"]):
			best = m
	return best

# Two hardnesses meeting inside one crater: the exact geometry that used to distort the field.
func build_seam(w: VoxelWorld) -> Vector3:
	# x sits at the CENTRE of occupancy cell 25 ([20.0, 20.8)), so the cells either side of
	# it are exact mirrors and a symmetry comparison means what it says.
	var centre := Vector3(20.4, 47.5, 20.0)
	var hard := hardest(w)
	var soft := softest(w)
	assert_float(float(hard["hardness"])).override_failure_message(
		"every material has the same hardness: this test cannot build a seam"
		).is_greater(float(soft["hardness"]))
	# A slab of the hardest material, then a slab of the softest, sharing a plane.
	w.hooks().debug_apply_sphere_paint(centre - Vector3(2.0, 0, 0), 2.0, int(hard["id"]))
	w.hooks().debug_apply_sphere_paint(centre + Vector3(2.0, 0, 0), 2.0, int(soft["id"]))
	settle(w)
	# One carve spanning both. debug_apply_sphere_subtract is the raw hook: it stores the
	# radius it is given, which is exactly what a tool-side scale produces. Whatever the
	# stored radius is, it must be the SAME on both sides of the plane.
	w.hooks().debug_apply_sphere_subtract(centre, 3.0)
	settle(w)
	return centre

# The invariant. One stored op describes one sphere, so the crater it leaves is symmetric
# about its centre no matter which materials it crosses. A per-sample hardness lookup makes
# the hard side stop short, which is the discontinuity everything else here is downstream of.
func test_one_carve_has_one_radius_across_a_material_seam() -> void:
	var w := make_world()
	var centre := build_seam(w)
	var cx := int(centre.x / 0.8)
	var cy := int(centre.y / 0.8)
	var cz := int(centre.z / 0.8)
	var mismatches := 0
	var compared := 0
	# Mirrored cell pairs about the carve centre. Cells 1..3 out reach 2.8 m, inside the
	# 3.0 m sphere on both sides, so every pair must report the same state. Under per-sample
	# hardness the hard side stopped at 1.0 m and pairs 2 and 3 disagreed.
	for i in range(1, 4):
		var left := w.hooks().debug_cell_state(Vector3i(cx - i, cy, cz))
		var right := w.hooks().debug_cell_state(Vector3i(cx + i, cy, cz))
		compared += 1
		if left != right:
			mismatches += 1
	assert_int(compared).is_equal(3)
	assert_int(mismatches).override_failure_message(
		"%d of %d matched cell pairs across the hardness seam disagree: the carve is not one sphere, so a per-sample hardness lookup is back" % [mismatches, compared]
		).is_equal(0)

# The marcher and the analytic raycast walk the same world by different means. A ray that
# leaks through a seam lip reports sky where the field says there is surface.
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
		"%d of %d rays across the hardness seam missed a surface the field reports" % [leaks, tested]
		).is_equal(0)

func terrain_height(x: float, z: float) -> float:
	return 51.2 + 6.0 * sin(x * 0.11) * cos(z * 0.13) \
		+ 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043) \
		+ sin(x * 0.23 + z * 0.19)

# Hardness must still DO something, or the symmetry test above passes trivially on a build
# where hardness was simply deleted. This is the end-to-end path the demo uses: the raycast
# reports the struck material, the tool turns it into one smaller sphere.
func test_the_tool_shrinks_a_removal_by_the_struck_material() -> void:
	var w := make_world()
	var t := make_tool(w)
	var hard := hardest(w)
	const NOMINAL := 3.0
	var expected_hard: float = NOMINAL / float(hard["hardness"])

	# Two sites far enough apart that neither crater reaches the other. Each is measured
	# against its OWN pre-carve surface, so the terrain's slope cancels out.
	var soft_at := Vector3(16.0, 0.0, 20.0)
	var hard_at := Vector3(26.0, 0.0, 20.0)

	var soft_before: Dictionary = w.hooks().debug_raycast(
		Vector3(soft_at.x, 70.0, soft_at.z), Vector3(0, -1, 0))
	var hard_before: Dictionary = w.hooks().debug_raycast(
		Vector3(hard_at.x, 70.0, hard_at.z), Vector3(0, -1, 0))
	assert_bool(bool(soft_before["hit"])).is_true()
	assert_bool(bool(hard_before["hit"])).is_true()

	# Material 0 is air, whose fail-soft hardness is 1.0: the compatibility default every
	# caller that passes no material gets.
	t.apply_sphere_subtract(soft_before["pos"], NOMINAL, 0)
	t.apply_sphere_subtract(hard_before["pos"], NOMINAL, int(hard["id"]))

	# Straight down through each centre: the crater floor is centre.y - effective_radius.
	var soft_after: Dictionary = w.hooks().debug_raycast(
		Vector3(soft_at.x, 70.0, soft_at.z), Vector3(0, -1, 0))
	var hard_after: Dictionary = w.hooks().debug_raycast(
		Vector3(hard_at.x, 70.0, hard_at.z), Vector3(0, -1, 0))
	assert_bool(bool(soft_after["hit"])).is_true()
	assert_bool(bool(hard_after["hit"])).is_true()

	var soft_depth: float = float(soft_before["pos"].y) - float(soft_after["pos"].y)
	var hard_depth: float = float(hard_before["pos"].y) - float(hard_after["pos"].y)
	# 5 cm is one voxel, which is the tracer's own resolution at the crater floor.
	assert_float(soft_depth).override_failure_message(
		"a material-0 carve should keep its full %.2f m radius, got %.2f m" % [NOMINAL, soft_depth]
		).is_equal_approx(NOMINAL, 0.05)
	assert_float(hard_depth).override_failure_message(
		"a carve into %s (hardness %.1f) should reach %.2f m, got %.2f m"
		% [hard["name"], float(hard["hardness"]), expected_hard, hard_depth]
		).is_equal_approx(expected_hard, 0.05)

# The ordinary "break a rock" case, end to end: a hard material carves a small crater, and
# the untouched surface just outside it must still march exactly where the closed-form
# terrain says it is. Comparing against the analytic height rather than against the CPU
# raycast matters -- both are sphere tracers, so one leak could hide the other.
func test_rays_do_not_leak_beside_a_hard_material_crater() -> void:
	var w := make_world()
	var t := make_tool(w)
	var hard := hardest(w)
	var centre := Vector3(20.0, 49.56, 20.0)
	w.hooks().debug_apply_sphere_paint(centre, 5.0, int(hard["id"]))
	settle(w)
	t.apply_sphere_subtract(centre, 3.0, int(hard["id"]))
	settle(w)

	var leaks := 0
	# The hard material carves to 3.0 / hardness. Rays in this annulus land outside that
	# crater, on the untouched procedural surface.
	var reach: float = 3.0 / float(hard["hardness"])
	assert_float(reach).override_failure_message(
		"the hardest material must actually shrink a 3 m carve for this fixture to mean anything"
		).is_less(1.8)
	for i in range(24):
		var angle := TAU * float(i) / 24.0
		var origin := centre + Vector3(cos(angle) * 1.8, 8.0, sin(angle) * 1.8)
		var marched: Dictionary = w.hooks().debug_raymarch_gbuffer(origin, Vector3(0, -1, 0))
		var expected_y := terrain_height(origin.x, origin.z)
		if not marched["hit"] or absf(float(marched["position"].y) - expected_y) > 0.15:
			leaks += 1
	assert_int(leaks).override_failure_message(
		"%d of 24 rays leaked beside a hard-material crater" % leaks).is_equal(0)
