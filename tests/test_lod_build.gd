extends GdUnitTestSuite

var _worlds: Array = []

# debug_lod_collect() never blocks (unlike debug_mesh_collect(), which waits for its batch),
# so every test here polls for its results. The budget is a ceiling on a FAILING run, not a
# target: a landing batch breaks the loop on the first frame it is ready, and a frame costs
# well under a millisecond now that the runner disables vsync. It used to be 300, which was
# minutes of slack only because each frame was blocking on an occluded swapchain.
const POLL_FRAMES := 4000

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
	assert_bool(w.debug_init_physics()).is_true()
	return w

func test_a_submitted_chunk_comes_back_with_quads(timeout := 20000) -> void:
	var w := make_world()
	# L0 chunk (2, 4, 2) straddles the surface at y ~ 51.2.
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 4, 2)]])).is_true()
	var got: Array = []
	for i in range(POLL_FRAMES):
		got = w.debug_lod_collect()
		if got.size() > 0:
			break
		await get_tree().process_frame
	assert_int(got.size()).override_failure_message(
		"the LoD build never came back").is_equal(1)
	assert_int(got[0]["level"]).is_equal(0)
	assert_vector(got[0]["coord"]).is_equal(Vector3i(2, 4, 2))
	assert_int(got[0]["quads"]).override_failure_message(
		"a chunk straddling the surface produced no quads").is_greater(0)
	assert_bool(got[0]["failed"]).is_false()

func test_an_air_chunk_comes_back_empty(timeout := 20000) -> void:
	var w := make_world()
	# High above the terrain: no surface, so no quads and no wasted pages.
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 12, 2)]])).is_true()
	var got: Array = []
	for i in range(POLL_FRAMES):
		got = w.debug_lod_collect()
		if got.size() > 0:
			break
		await get_tree().process_frame
	assert_int(got.size()).override_failure_message(
		"the empty-chunk LoD build never came back").is_equal(1)
	assert_int(got[0]["quads"]).is_equal(0)

func test_multi_job_batch_returns_all_results(timeout := 20000) -> void:
	var w := make_world()
	# Both chunks straddle the surface, and they are submitted together in one plural batch.
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 4, 2)], [0, Vector3i(3, 4, 2)]])).is_true()
	var got: Array = []
	for i in range(POLL_FRAMES):
		got = w.debug_lod_collect()
		if got.size() >= 2:
			break
		await get_tree().process_frame
	assert_int(got.size()).override_failure_message(
		"the multi-job LoD batch never came back complete").is_equal(2)
	assert_vector(got[0]["coord"]).is_equal(Vector3i(2, 4, 2))
	assert_vector(got[1]["coord"]).is_equal(Vector3i(3, 4, 2))
	for r in got:
		assert_int(r["quads"]).override_failure_message(
			"a multi-job LoD batch dropped or emptied a chunk").is_greater(0)
		assert_bool(r["failed"]).is_false()

func test_a_batch_is_refused_while_one_is_in_flight() -> void:
	var w := make_world()
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 4, 2)]])).is_true()
	# MeshPass's one-batch-at-a-time contract: the residency bookkeeping relies on it.
	assert_bool(w.debug_lod_submit([[0, Vector3i(3, 4, 2)]])).is_false()

func test_collider_meshing_still_works_alongside(timeout := 20000) -> void:
	var w := make_world()
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 4, 2)]])).is_true()
	assert_bool(w.debug_mesh_submit([Vector3i(4, 8, 4)])).is_true()
	var lod_done := false
	var mesh_done := false
	for i in range(POLL_FRAMES):
		if w.debug_lod_collect().size() > 0:
			lod_done = true
		if w.debug_mesh_collect().size() > 0:
			mesh_done = true
		if lod_done and mesh_done:
			break
		await get_tree().process_frame
	assert_bool(lod_done).override_failure_message("the LoD queue starved").is_true()
	assert_bool(mesh_done).override_failure_message("the collider queue starved").is_true()
