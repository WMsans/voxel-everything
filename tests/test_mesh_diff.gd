extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# GPU/CPU differential test for the collision mesher (spec section 8): shaders/mesh_cells and
# shaders/mesh_quads against ve::dual_contour. What is compared, and why each tolerance is
# what it is:
#
#  * The lattice, all 66^3 samples, against ve::eval_field: one encoded step, exactly as
#    test_brick_diff.gd allows (glibc's sin() vs the driver's).
#  * The MESH, against ve::dual_contour run on the GPU'S OWN read-back lattice. Both sides
#    therefore consume identical bytes, so the cell sets and triangle sets must match
#    EXACTLY and positions to 1 mm — the only remaining difference is float rounding inside
#    the interpolation. (This is M2 errata 7's rule for the mip chain applied to the mesher:
#    the property under test is that the algorithm agrees, not that sin() is bit-identical.)
#  * Triangles are compared as cyclically normalised triples of CELL indices, so vertex
#    numbering (the GPU allocates it with atomics, in no fixed order) does not enter, but an
#    inverted winding still shows up as a difference.
#  * Winding is checked independently: the field must be greater on the normal's side of
#    every triangle. That is what keeps a character on top of the ground rather than under it.

# A collision chunk is 8 bricks (6.4 m) on a side, sampled at 0.1 m; it was 16 bricks (12.8 m)
# until the collision streamer was profiled. Chunk coordinates below are therefore in units of
# 6.4 m. SURFACE_CHUNK spans world y [44.8, 51.2) over x,z [25.6, 32.0): measured, the
# terrain crosses it with ~17k triangles over ~8.9k cells, so the thresholds below still mean
# "a substantial surface" and did not have to be weakened when the chunk shrank. The chunk
# directly above it only clips the surface (~430 triangles) and would have.
const CHUNK_M := 6.4
const SURFACE_CHUNK := Vector3i(4, 7, 4)
# Origin (25.6, 64.0, 25.6): above the terrain, which tops out at 51.2 + 10.
const SKY_CHUNK := Vector3i(4, 10, 4)

func chunk_of(p: Vector3) -> Vector3i:
	return Vector3i(int(floor(p.x / CHUNK_M)), int(floor(p.y / CHUNK_M)),
		int(floor(p.z / CHUNK_M)))

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func check_diff(d: Dictionary, label: String) -> void:
	assert_int(d["lattice_max_diff"]).override_failure_message(
		"%s: lattice differs by %d encoded steps" % [label, d["lattice_max_diff"]]).is_less_equal(1)
	assert_int(d["cells_only_cpu"]).override_failure_message(
		"%s: %d cells hold a vertex on the CPU only" % [label, d["cells_only_cpu"]]).is_equal(0)
	assert_int(d["cells_only_gpu"]).override_failure_message(
		"%s: %d cells hold a vertex on the GPU only" % [label, d["cells_only_gpu"]]).is_equal(0)
	assert_float(d["max_pos_diff"]).override_failure_message(
		"%s: vertex positions differ by %f m" % [label, d["max_pos_diff"]]).is_less(0.001)
	assert_int(d["tri_only_cpu"]).override_failure_message(
		"%s: %d triangles are CPU-only" % [label, d["tri_only_cpu"]]).is_equal(0)
	assert_int(d["tri_only_gpu"]).override_failure_message(
		"%s: %d triangles are GPU-only" % [label, d["tri_only_gpu"]]).is_equal(0)
	assert_bool(d["overflow"]).override_failure_message(
		"%s: the mesher hit a per-chunk cap" % label).is_false()

func test_a_surface_chunk_meshes_identically_on_both_sides() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_mesh_diff(SURFACE_CHUNK)
	check_diff(d, "plain terrain")
	assert_int(d["cells_both"]).is_greater(1000)
	assert_int(d["tri_gpu"]).is_greater(1000)
	assert_int(d["tri_gpu"]).is_equal(d["tri_cpu"])

func test_every_vertex_sits_on_the_surface_and_faces_the_air() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_mesh_diff(SURFACE_CHUNK)
	# The generator reports a distance that exceeds the true one by at most lipschitz() = 2,
	# so "within 0.1 m reported" is "within half a 0.1 m cell of the real surface". The one
	# percent that may miss it are the cells straddling the crease where the cave sphere
	# meets the terrain: a mass point averaged across a kink lands slightly off both sheets.
	assert_int(d["verts_off_10cm"]).override_failure_message(
		"%d of %d vertices sit more than 0.1 m off the surface"
		% [d["verts_off_10cm"], d["cells_gpu"]]).is_less_equal(int(d["cells_gpu"] / 100))
	assert_float(d["max_surface_sdf"]).override_failure_message(
		"a vertex sits %f m (reported) off the surface" % d["max_surface_sdf"]).is_less(0.25)
	# A wholly inverted mesh would put EVERY sampled triangle here; the crease can account
	# for a handful.
	assert_int(d["winding_bad"]).override_failure_message(
		"%d of %d sampled triangles face into the solid"
		% [d["winding_bad"], d["tri_sampled"]]).is_less_equal(int(d["tri_sampled"] / 50))

func test_a_carved_chunk_still_matches() -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var before: Dictionary = w.debug_mesh_diff(SURFACE_CHUNK)
	var hit: Dictionary = w.debug_raycast(Vector3(30.0, 80.0, 30.0), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var r: Dictionary = tool.apply_sphere_subtract(hit["pos"], 3.0)
	assert_array(r["rejected"]).is_empty()

	var after: Dictionary = w.debug_mesh_diff(chunk_of(Vector3(30.0, hit["pos"].y, 30.0)))
	check_diff(after, "carved terrain")
	assert_int(after["op_count"]).is_greater(0)
	assert_int(after["tri_gpu"]).is_greater(0)
	assert_int(before["tri_gpu"]).is_greater(0)

func test_open_sky_meshes_to_nothing() -> void:
	var w := make_world()
	# Chunk (4, 10, 4) spans world y [64.0, 70.4); the surface tops out at 51.2 + 10.
	var d: Dictionary = w.debug_mesh_diff(SKY_CHUNK)
	assert_int(d["tri_gpu"]).is_equal(0)
	assert_int(d["tri_cpu"]).is_equal(0)
	assert_int(d["cells_both"]).is_equal(0)
	assert_int(d["cells_only_gpu"]).is_equal(0)
