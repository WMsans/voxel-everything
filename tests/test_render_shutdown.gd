extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func test_shutdown_drains_pending_hiz_readback_before_freeing_resources() -> void:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = true
	world.physics_enabled = false
	add_child(world)
	_worlds.append(world)

	var result: Dictionary = world.debug_hiz_shutdown_probe()
	assert_bool(result["callback_guarded"]).is_true()
	assert_bool(result["queued"]).is_true()
	assert_bool(result["was_pending"]).is_true()
	assert_bool(result["drained"]).is_true()
	assert_bool(result["initialized_after"]).is_false()
	world.ensure_initialized()
	assert_bool(world.is_initialized()).is_false()

# --- Task 8: teardown/reinit telemetry for the compact-normal pool ------------

func _packed_normals(seed: int) -> PackedByteArray:
	var samples := 17 * 17 * 17 # kBrickSdfCount
	var bytes := PackedByteArray()
	bytes.resize(samples * 2)
	for i in range(samples):
		@warning_ignore("integer_division")
		var v: int = (seed * 2654435761 + i * 97) % 65536
		bytes.encode_u16(i * 2, v)
	return bytes

func _assert_normal_invariants(stats: Dictionary) -> void:
	assert_bool(stats.has("normal_capacity_bytes")).is_true()
	assert_bool(stats.has("normal_live_bytes")).is_true()
	assert_bool(stats.has("normal_high_water_bytes")).is_true()
	assert_bool(stats.has("normal_allocation_failures")).is_true()
	assert_bool(stats.has("normal_fallback_hits")).is_true()
	var cap := int(stats["normal_capacity_bytes"])
	var live := int(stats["normal_live_bytes"])
	var hw := int(stats["normal_high_water_bytes"])
	assert_int(cap).is_greater(0)
	assert_int(live).is_greater_equal(0)
	assert_int(hw).is_greater_equal(0)
	assert_int(live).is_less_equal(cap)
	assert_int(hw).is_less_equal(cap)
	assert_int(live).is_less_equal(hw)

func test_render_teardown_invalidates_all_normal_pool_rids_and_reinit_resets_tables() -> void:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = true
	world.physics_enabled = false
	add_child(world)
	_worlds.append(world)
	assert_bool(world.debug_init_atlas()).is_true()

	var before: Dictionary = world.debug_stored_normal_stats()
	_assert_normal_invariants(before)
	assert_int(before["normal_capacity_bytes"]).is_equal(33554432)
	var pool: Dictionary = world.debug_normal_pool_state()
	assert_bool(pool["pool_valid"]).is_true()
	assert_bool(pool["normal_rid_valid"]).is_true()
	assert_bool(pool["volume_offsets_rid_valid"]).is_true()
	assert_bool(pool["override_offsets_rid_valid"]).is_true()
	assert_bool(pool["volume_offsets_all_minus_one"]).is_true()
	assert_bool(pool["override_offsets_all_minus_one"]).is_true()

	# A published override makes the override table non-empty before teardown.
	assert_int(world.debug_normal_upload_override(0, _packed_normals(1))).is_greater_equal(0)
	var published: Dictionary = world.debug_normal_pool_state()
	assert_bool(published["override_offsets_all_minus_one"]).is_false()
	assert_bool(published["volume_offsets_all_minus_one"]).is_true()

	world.debug_teardown_atlas()
	var torn: Dictionary = world.debug_normal_pool_state()
	assert_bool(torn["pool_valid"]).is_false()
	assert_bool(torn["normal_rid_valid"]).is_false()
	assert_bool(torn["volume_offsets_rid_valid"]).is_false()
	assert_bool(torn["override_offsets_rid_valid"]).is_false()
	# The atlas is gone, so no stored-normal telemetry remains published.
	assert_bool(world.debug_stored_normal_stats().is_empty()).is_true()

	# Reinitialization restores the offset tables to -1: fresh buffers, no spans.
	assert_bool(world.debug_init_atlas()).is_true()
	var reinit: Dictionary = world.debug_normal_pool_state()
	assert_bool(reinit["pool_valid"]).is_true()
	assert_bool(reinit["normal_rid_valid"]).is_true()
	assert_bool(reinit["volume_offsets_rid_valid"]).is_true()
	assert_bool(reinit["override_offsets_rid_valid"]).is_true()
	assert_bool(reinit["volume_offsets_all_minus_one"]).is_true()
	assert_bool(reinit["override_offsets_all_minus_one"]).is_true()
	var after: Dictionary = world.debug_stored_normal_stats()
	_assert_normal_invariants(after)
	assert_int(after["normal_live_bytes"]).is_equal(0)
	assert_int(after["normal_capacity_bytes"]).is_equal(33554432)

func test_physics_teardown_preserves_the_normal_pool_and_its_invariants(timeout := 60000) -> void:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = true
	world.physics_enabled = false
	world.world_origin_bricks = Vector3i(0, -64, 0)
	world.world_size_regions = Vector3i(8, 5, 8)
	world.residency_radius_m = 40.0
	world.atlas_bricks = Vector3i(32, 16, 32)
	world.max_region_slots = 64
	add_child(world)
	_worlds.append(world)
	assert_bool(world.debug_init_physics()).is_true()
	for i in range(60):
		world.debug_stream_frame(Vector3(20.0, 56.0, 20.0))
	var d: Dictionary = world.debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var before: Dictionary = world.debug_stored_normal_stats()
	_assert_normal_invariants(before)
	assert_int(before["normal_live_bytes"]).is_greater(0)

	world.debug_teardown_physics()
	var after: Dictionary = world.debug_stored_normal_stats()
	_assert_normal_invariants(after)
	assert_int(after["normal_capacity_bytes"]).is_equal(int(before["normal_capacity_bytes"]))
	# Physics teardown must not touch the GPU pool: RIDs stay valid, high-water never drops.
	var pool: Dictionary = world.debug_normal_pool_state()
	assert_bool(pool["pool_valid"]).is_true()
	assert_bool(pool["normal_rid_valid"]).is_true()
	assert_bool(pool["volume_offsets_rid_valid"]).is_true()
	assert_bool(pool["override_offsets_rid_valid"]).is_true()
	assert_int(after["normal_high_water_bytes"]).is_equal(int(before["normal_high_water_bytes"]))
	assert_bool(world.debug_init_physics()).is_true()
	var reinit: Dictionary = world.debug_stored_normal_stats()
	_assert_normal_invariants(reinit)
	assert_int(reinit["normal_capacity_bytes"]).is_equal(int(before["normal_capacity_bytes"]))
