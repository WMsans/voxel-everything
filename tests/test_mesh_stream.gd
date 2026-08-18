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

# Chunk coordinates are in units of the 6.4 m collision chunk (8 bricks); it was 12.8 m until
# the collision streamer was profiled. The three surface chunks all span world y [44.8, 51.2)
# and were chosen by measurement — each carries several thousand triangles, so the counts
# asserted below still mean "a real surface". SKY_CHUNK sits above the terrain and meshes to
# nothing.
const SURFACE_CHUNK := Vector3i(4, 7, 4)
const SURFACE_CHUNK_2 := Vector3i(10, 7, 4)
const SURFACE_CHUNK_3 := Vector3i(8, 7, 4)
const SKY_CHUNK := Vector3i(4, 10, 4)

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
	assert_bool(w.debug_mesh_submit([SURFACE_CHUNK, SURFACE_CHUNK_2])).is_true()
	# A second batch cannot start while one is in flight.
	assert_bool(w.debug_mesh_submit([SURFACE_CHUNK_3])).is_false()

	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(2)
	assert_object(got[0]["chunk"]).is_equal(SURFACE_CHUNK)
	assert_object(got[1]["chunk"]).is_equal(SURFACE_CHUNK_2)
	for r in got:
		assert_int(r["triangles"]).is_greater(1000)
		assert_int(r["vertices"]).is_greater(500)
		assert_bool(r["overflow"]).is_false()

	# Nothing is left to collect, and the pass is free again.
	assert_int(w.debug_mesh_collect().size()).is_equal(0)
	assert_bool(w.debug_mesh_submit([SURFACE_CHUNK_3])).is_true()

func test_a_batch_agrees_with_the_synchronous_path() -> void:
	var w := make_world()
	var one: Dictionary = w.debug_mesh_diff(SURFACE_CHUNK)
	assert_bool(w.debug_mesh_submit([SURFACE_CHUNK])).is_true()
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(1)
	# The batch path shares every buffer and every dispatch with the inline path; the counts
	# must be identical, or a job's state is leaking between the two.
	assert_int(got[0]["triangles"]).is_equal(one["tri_gpu"])
	assert_int(got[0]["vertices"]).is_equal(one["cells_gpu"])

func test_jobs_in_one_batch_do_not_leak_into_each_other() -> void:
	var w := make_world()
	# SKY_CHUNK is open sky and meshes to nothing; batching it with a surface chunk must not
	# give it the other job's triangles (they share one lattice and one cell map).
	assert_bool(w.debug_mesh_submit([SURFACE_CHUNK, SKY_CHUNK])).is_true()
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(2)
	assert_int(got[0]["triangles"]).is_greater(1000)
	assert_int(got[1]["triangles"]).is_equal(0)
	assert_int(got[1]["vertices"]).is_equal(0)

# The diagnostic sync path shares the pass's single lattice and cell map with the streaming
# path, so the two must never run at once. It used to REFUSE while a batch was outstanding,
# back when meshing happened inline on the calling thread. Now that the mesher owns a worker
# thread, MeshService::run_sync waits for its turn instead — a better contract, and the one
# pinned here. What must still hold is the reason the refusal existed: the diagnostic does not
# corrupt the batch it interleaved with.
func test_inline_sync_waits_for_a_batch_in_flight() -> void:
	var w := make_world()
	var reference: Dictionary = w.debug_mesh_diff(SURFACE_CHUNK)
	assert_int(reference["tri_gpu"]).is_greater(1000)

	assert_bool(w.debug_mesh_submit([SURFACE_CHUNK])).is_true()
	# Runs on the worker thread, behind the batch, and returns real data rather than nothing.
	var d: Dictionary = w.debug_mesh_lattice_diff(SURFACE_CHUNK_2)
	assert_bool(d.has("samples")).override_failure_message(
		"the diagnostic returned nothing: %s" % d).is_true()

	# The batch is intact: had the diagnostic run concurrently it would have overwritten the
	# lattice the batch was meshing from, and these counts would not match a clean run.
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(1)
	assert_bool(got[0]["failed"]).is_false()
	assert_int(got[0]["triangles"]).override_failure_message(
		"an interleaved diagnostic changed the batch: %d triangles vs %d clean"
		% [got[0]["triangles"], reference["tri_gpu"]]).is_equal(reference["tri_gpu"])

# MeshPass meshes at most config().max_jobs chunks per batch (mesh_jobs_per_frame, 2 here).
# An oversized batch is not rejected at submit: the worker reports a FAILURE PER CHUNK, so the
# caller can clear the in-flight marker it holds for each one. A silent drop would strand them
# for ever, which is the failure mode this pins.
func test_an_oversized_batch_fails_every_chunk_instead_of_dropping_it() -> void:
	var w := make_world()
	var over := [SURFACE_CHUNK, SURFACE_CHUNK_2, SURFACE_CHUNK_3]
	assert_bool(w.debug_mesh_submit(over)).is_true()
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).override_failure_message(
		"an oversized batch stranded %d of its %d chunks" % [over.size() - got.size(), over.size()]
		).is_equal(over.size())
	for i in range(got.size()):
		assert_object(got[i]["chunk"]).is_equal(over[i])
		assert_bool(got[i]["failed"]).override_failure_message(
			"chunk %s came back from an oversized batch as a success" % got[i]["chunk"]).is_true()
		# A failed chunk carries no geometry, so nothing partial reaches Jolt.
		assert_int(got[i]["triangles"]).is_equal(0)
	# ...and the refusal does not wedge the queue: a legal batch still runs afterwards.
	assert_bool(w.debug_mesh_submit([SURFACE_CHUNK])).is_true()
	var after: Array = w.debug_mesh_collect()
	assert_int(after.size()).is_equal(1)
	assert_bool(after[0]["failed"]).is_false()
	assert_int(after[0]["triangles"]).is_greater(1000)
