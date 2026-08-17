extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(pages: int = 256) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.max_lod_pages = pages
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func test_the_pool_starts_empty_and_sized() -> void:
	var w := make_world(256)
	var d := w.debug_lod_stats()
	assert_int(d["pages_total"]).is_equal(256)
	assert_int(d["pages_free"]).is_equal(256)
	assert_int(d["chunks_resident"]).is_equal(0)

func test_ticking_streams_chunks_in(timeout := 30000) -> void:
	var w := make_world(2048)
	for i in range(200):
		w.debug_lod_tick(Vector3(400.0, 70.0, 400.0), Vector3(0, -0.2, -1))
		await get_tree().process_frame
	var d := w.debug_lod_stats()
	assert_int(d["chunks_resident"]).override_failure_message(
		"200 ticks produced no resident chunks: %s" % d).is_greater(0)
	assert_int(d["pages_free"]).is_less(2048)
	assert_int(d["draw_pages"]).override_failure_message(
		"chunks are resident but nothing is in the draw list").is_greater(0)

# M3 errata 5's lesson, restated for pages: a build that cannot get all its pages must be
# refused, never half-allocated. A tiny pool must degrade to a coarse world, not a broken one.
func test_a_tiny_pool_degrades_to_coarse_instead_of_breaking(timeout := 30000) -> void:
	var w := make_world(24)
	for i in range(200):
		w.debug_lod_tick(Vector3(400.0, 70.0, 400.0), Vector3(0, -0.2, -1))
		await get_tree().process_frame
	var d := w.debug_lod_stats()
	assert_int(d["pages_free"]).is_greater_equal(0)
	assert_int(d["pages_used"] as int + d["pages_free"] as int).is_equal(24)
	assert_bool(d["partial_allocations"] as int == 0).override_failure_message(
		"a build was partially funded").is_true()
	# Something is still drawn: the coarse levels are exempt from eviction.
	assert_int(d["draw_pages"]).is_greater(0)

# LoD frames under the display-capable wrapper are much slower on this GPU
# environment than the brief's original 30 s assumption; 10 minutes gives the
# 400 eviction frames enough wall-clock room to finish.
func test_pages_come_back_when_chunks_are_evicted(timeout := 600000) -> void:
	var w := make_world(2048)
	for i in range(120):
		w.debug_lod_tick(Vector3(400.0, 70.0, 400.0), Vector3(0, -0.2, -1))
		await get_tree().process_frame
	var used_near: int = w.debug_lod_stats()["pages_used"]
	assert_int(used_near).is_greater(0)
	# Jump far away and let the eviction age expire.
	for i in range(400):
		w.debug_lod_tick(Vector3(1500.0, 400.0, 1500.0), Vector3(0, -1, 0))
		await get_tree().process_frame
	var d := w.debug_lod_stats()
	assert_int(d["pages_used"]).override_failure_message(
		"nothing was ever evicted: pages_used %d -> %d" % [used_near, d["pages_used"]]
		).is_less(used_near)
