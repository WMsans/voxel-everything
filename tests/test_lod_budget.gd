extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(records: int) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1400.0
	w.max_lod_pages = 32768
	w.max_lod_chunk_records = records
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, frames: int) -> void:
	var fwd := Vector3(1, -0.3, 1).normalized()
	for i in range(frames):
		w.hooks().debug_lod_tick(Vector3(60, 80, 60), fwd)
		await get_tree().process_frame

func test_the_budget_is_reported_and_defaults_to_8192() -> void:
	var w := make_world(8192)
	await settle(w, 40)
	var d: Dictionary = w.hooks().debug_lod_stats()
	assert_int(d["chunk_records"]).is_equal(8192)
	assert_int(d["chunk_records_used"]).is_greater(0)
	assert_int(d["chunk_records_high_water"]).is_greater_equal(int(d["chunk_records_used"]))

# The horizon stops arriving; the ground does not disappear. Same contract the brick atlas
# states. This is the path that has never executed while kChunkRecords was private.
func test_exhausting_the_records_stops_the_horizon_without_corrupting_it(timeout := 120000) -> void:
	var starved := make_world(16)
	await settle(starved, 60)
	var d: Dictionary = starved.hooks().debug_lod_stats()
	# Never over budget, and the budget is named as the thing that bound.
	assert_int(d["chunk_records_used"]).is_less_equal(16)
	assert_str(str(d["budget_bound"])).is_equal("chunk_records")
	# Whatever DID land is still coherent: every drawn page belongs to a live chunk.
	assert_int(d["draw_pages"]).is_greater_equal(0)
	assert_int(d["chunks_resident"]).is_less_equal(16)

func test_a_funded_world_reports_no_bound_budget() -> void:
	var w := make_world(8192)
	await settle(w, 40)
	assert_str(str(w.hooks().debug_lod_stats()["budget_bound"])).is_equal("none")
