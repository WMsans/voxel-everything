extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# The mesher's pipeline contract: one batch in flight, submitted now and collected later, so
# no frame ever waits on the GPU. Everything here runs on the mesher's own local device — the
# renderer's device is not even initialised.

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.mesh_jobs_per_frame = 2
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func test_a_batch_is_submitted_once_and_collected_once() -> void:
	var w := make_world()
	assert_bool(w.debug_mesh_submit([Vector3i(2, 4, 2), Vector3i(3, 4, 2)])).is_true()
	# A second batch cannot start while one is in flight.
	assert_bool(w.debug_mesh_submit([Vector3i(4, 4, 2)])).is_false()

	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(2)
	assert_object(got[0]["chunk"]).is_equal(Vector3i(2, 4, 2))
	assert_object(got[1]["chunk"]).is_equal(Vector3i(3, 4, 2))
	for r in got:
		assert_int(r["triangles"]).is_greater(1000)
		assert_int(r["vertices"]).is_greater(500)
		assert_bool(r["overflow"]).is_false()

	# Nothing is left to collect, and the pass is free again.
	assert_int(w.debug_mesh_collect().size()).is_equal(0)
	assert_bool(w.debug_mesh_submit([Vector3i(4, 4, 2)])).is_true()

func test_a_batch_agrees_with_the_synchronous_path() -> void:
	var w := make_world()
	var one: Dictionary = w.debug_mesh_diff(Vector3i(2, 4, 2))
	assert_bool(w.debug_mesh_submit([Vector3i(2, 4, 2)])).is_true()
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(1)
	# The batch path shares every buffer and every dispatch with the inline path; the counts
	# must be identical, or a job's state is leaking between the two.
	assert_int(got[0]["triangles"]).is_equal(one["tri_gpu"])
	assert_int(got[0]["vertices"]).is_equal(one["cells_gpu"])

func test_jobs_in_one_batch_do_not_leak_into_each_other() -> void:
	var w := make_world()
	# Chunk (2, 5, 2) is open sky and meshes to nothing; batching it with a surface chunk
	# must not give it the other job's triangles (they share one lattice and one cell map).
	assert_bool(w.debug_mesh_submit([Vector3i(2, 4, 2), Vector3i(2, 5, 2)])).is_true()
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(2)
	assert_int(got[0]["triangles"]).is_greater(1000)
	assert_int(got[1]["triangles"]).is_equal(0)
	assert_int(got[1]["vertices"]).is_equal(0)

func test_inline_sync_is_refused_while_a_batch_is_in_flight() -> void:
	var w := make_world()
	assert_bool(w.debug_mesh_submit([Vector3i(2, 4, 2)])).is_true()
	# The diagnostic sync path must not start a second batch on the same local device while
	# a streaming batch is outstanding.
	var d: Dictionary = w.debug_mesh_lattice_diff(Vector3i(3, 4, 2))
	assert_bool(d.has("samples")).is_false()
	# The batch is still collectable afterwards.
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(1)

func test_an_oversized_batch_is_refused() -> void:
	var w := make_world()
	assert_bool(w.debug_mesh_submit(
		[Vector3i(2, 4, 2), Vector3i(3, 4, 2), Vector3i(4, 4, 2)])).is_false()
	assert_int(w.debug_mesh_collect().size()).is_equal(0)
