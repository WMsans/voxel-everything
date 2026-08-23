extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# GPU/CPU differential test for the LoD build (engine spec section 8), the M5 counterpart of
# test_mesh_diff.gd. Three things are compared and each tolerance is what it is for a reason:
#
#  * The FINE lattice (69^3) against ve::eval_field: one encoded step, exactly as
#    test_brick_diff.gd allows (glibc's sin() against the driver's).
#  * The REDUCED lattice (34^3) against ve::lod_reduce_lattice run on the GPU's own fine
#    lattice. Both sides consume identical bytes, so this must agree to one encoded step and
#    the material lattice must agree EXACTLY -- a vote has no rounding.
#  * The QUADS against ve::lod_contour run on the GPU's own reduced lattice, compared as sets
#    of (u, axis) with their four corner offsets, so quad emission order (the GPU allocates
#    with atomics, in no fixed order) does not enter but a wrong winding still shows up.

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func check_diff(d: Dictionary, label: String) -> void:
	assert_bool(d.has("fine_max_diff")).override_failure_message(
		"%s: debug_lod_diff returned %s" % [label, d]).is_true()
	assert_int(d["fine_max_diff"]).override_failure_message(
		"%s: fine lattice differs by %d encoded steps" % [label, d["fine_max_diff"]]
		).is_less_equal(1)
	assert_int(d["reduced_max_diff"]).override_failure_message(
		"%s: reduced lattice differs by %d encoded steps" % [label, d["reduced_max_diff"]]
		).is_less_equal(1)
	assert_int(d["material_mismatches"]).override_failure_message(
		"%s: %d reduced material samples disagree" % [label, d["material_mismatches"]]
		).is_equal(0)
	assert_int(d["quads_only_cpu"]).override_failure_message(
		"%s: %d quads exist on the CPU only" % [label, d["quads_only_cpu"]]).is_equal(0)
	assert_int(d["quads_only_gpu"]).override_failure_message(
		"%s: %d quads exist on the GPU only" % [label, d["quads_only_gpu"]]).is_equal(0)
	assert_int(d["corner_max_diff"]).override_failure_message(
		"%s: a corner offset differs by %d steps" % [label, d["corner_max_diff"]]).is_equal(0)

func test_level_zero_over_the_surface() -> void:
	var w := make_world()
	# The terrain surface sits at y ~ 51.2 + hills (M2 errata 9), so an L0 chunk (12.8 m)
	# whose y index is 4 straddles it.
	check_diff(w.hooks().debug_lod_diff(0, Vector3i(2, 4, 2)), "L0 surface")

func test_every_level_agrees() -> void:
	var w := make_world()
	for level in range(0, 8):
		# The chunk containing (25.6, 51.2, 25.6) at each level.
		var s := 12.8 * pow(2.0, float(level))
		var c := Vector3i(int(floor(25.6 / s)), int(floor(51.2 / s)), int(floor(25.6 / s)))
		check_diff(w.hooks().debug_lod_diff(level, c), "level %d" % level)

func test_an_edit_reaches_the_coarse_levels() -> void:
	var w := make_world()
	# A 5 m crater. At L4 the cell is 6.4 m, so the crater is under one cell -- and the whole
	# point of the half-cell supersample is that it still moves samples there. Point sampling
	# at 6.4 m would leave the coarse lattice bit-identical, which this asserts against.
	var before := w.hooks().debug_lod_diff(4, Vector3i(0, 0, 0))
	assert_bool(before.has("reduced_hash")).is_true()
	var hash_before: int = before["reduced_hash"]
	w.hooks().debug_apply_sphere_subtract(Vector3(25.6, 51.2, 25.6), 5.0)
	var after := w.hooks().debug_lod_diff(4, Vector3i(0, 0, 0))
	check_diff(after, "L4 after a 5 m crater")
	assert_int(after["reduced_hash"]).override_failure_message(
		"a 5 m crater left the 6.4 m lattice bit-identical: the reduction is point sampling"
		).is_not_equal(hash_before)
