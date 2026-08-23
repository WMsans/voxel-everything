extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# GPU/CPU differential test for the collision chunk lattice (spec section 8). The mesher does
# not read the brick atlas: it evaluates shaders/field.glslh at 0.1 m, the same field the
# bricks are generated from, so collision cannot inherit a dropped brick's hole and a chunk
# outside the residency ball can still be meshed.
#
# Tolerance: one encoded step, for the same reason test_brick_diff.gd allows one — sin() is
# not bit-identical between glibc and a Vulkan driver, and a uint8 with ~5 mm steps cannot
# show a disagreement smaller than half a step. Two steps would be a real bug.

# A collision chunk is kChunkBricks = 8 bricks (6.4 m) sampled at kChunkCellSize = 0.1 m, so
# kChunkCells = 64 and the lattice — which carries one cell of overlap below the chunk origin
# on every axis — is kChunkCells + 2 = 66 on a side. It was twice that on both counts until
# the collision streamer was profiled and the chunk edge was halved. The numbers are spelled
# out rather than derived so that changing the chunk again fails HERE, where the geometry is
# explained.
const CHUNK_M := 6.4
const LATTICE := 66

# The chunk whose origin is (25.6, 51.2, 25.6): the terrain surface (51.2 + hills) crosses it.
const SURFACE_CHUNK := Vector3i(4, 8, 4)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false          # the tests drive the tick by hand
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func test_lattice_matches_the_cpu_field() -> void:
	var w := make_world()
	# Chunk (4, 8, 4) spans world y [51.2, 57.6) — the surface (51.2 + hills) crosses it.
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(SURFACE_CHUNK)
	assert_int(d["samples"]).is_equal(LATTICE * LATTICE * LATTICE)
	assert_int(d["max_diff"]).override_failure_message(
		"lattice differs by %d encoded steps" % d["max_diff"]).is_less_equal(1)
	assert_int(d["diff_over_one"]).is_equal(0)
	# The chunk really does straddle the surface, or the comparison proved nothing.
	assert_bool(d["has_surface"]).is_true()

func test_lattice_includes_the_overlap_plane_and_the_edits() -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	# Carve at the low corner of the surface chunk so the crater reaches into the overlap cell
	# below its origin — the plane a chunk needs to close its minimum faces.
	var origin := Vector3(SURFACE_CHUNK) * CHUNK_M
	var r: Dictionary = tool.apply_sphere_subtract(origin + Vector3(0.05, 0.05, 0.05), 3.0)
	assert_array(r["rejected"]).is_empty()
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(SURFACE_CHUNK)
	assert_int(d["max_diff"]).is_less_equal(1)
	assert_int(d["diff_over_one"]).is_equal(0)
	assert_int(d["op_count"]).is_greater(0)

func test_a_chunk_far_outside_the_residency_ball_still_meshes() -> void:
	var w := make_world()
	# Nothing is streamed, no atlas is initialised: the mesher is independent of both.
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(Vector3i(14, 8, 14))
	assert_int(d["samples"]).is_equal(LATTICE * LATTICE * LATTICE)
	assert_int(d["max_diff"]).is_less_equal(1)
