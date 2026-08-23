extends GdUnitTestSuite

# Phase-2 contract smoke test (Task 10): accepted edits bump edit_seq monotonically
# across the WorldStore boundary, and the WorldStore-owned FieldGenerator seam answers
# samples for the same analytic terrain the CPU evaluators have always walked.

const OP_SUBTRACT := 0

var _world: VoxelWorld

func after_test() -> void:
	if _world != null and is_instance_valid(_world):
		_world.free()
	_world = null

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	# Byte-identical to ve::EditOp: type, material, pos[3], radius, pad[2] — 32 bytes.
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type)
	b.put_u32(material)
	b.put_float(pos.x)
	b.put_float(pos.y)
	b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0)
	b.put_u32(0)
	return b.data_array

func sphere_subtract_op() -> PackedByteArray:
	return make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 6.0)

func test_append_bumps_edit_seq_monotonically() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.use_local_device = true
	add_child(_world)
	_world.ensure_initialized()
	var before: int = _world.edit_seq()
	var op := sphere_subtract_op()
	var result: Dictionary = _world.append_edit(op)
	assert_array(result["rejected"]).is_empty()
	assert_array(result["touched"]).is_not_empty()
	assert_int(_world.edit_seq()).is_greater(before)

# Phase-3 contract smoke test (Task 11): refusal/success counters surface through the stats
# with unchanged semantics now that ConsolidationCoordinator owns them. Same hooks-facade
# shape test_deferred.gd uses; same counters test_consolidation.gd has always asserted.
func test_consolidation_refusal_accounting() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.use_local_device = true
	_world.physics_enabled = false
	_world.world_origin_bricks = Vector3i(0, -64, 0)
	_world.world_size_regions = Vector3i(8, 5, 8)
	add_child(_world)
	_world.ensure_initialized()
	_world.hooks().debug_stream_region(Vector3i(0, 2, 0))
	var stats: Dictionary = _world.hooks().debug_stream_stats()
	var refusals_before: int = int(stats.get("consolidation_refusals", 0))
	var successes_before: int = int(stats.get("consolidations", 0))
	# Refusal path: region (1,2,1) was never streamed, so it has no residency slot; the
	# forced consolidation must refuse once and bump exactly the refusal counter.
	assert_bool(_world.hooks().debug_consolidate_region(Vector3i(1, 2, 1))).is_false()
	stats = _world.hooks().debug_stream_stats()
	assert_int(int(stats["consolidation_refusals"])).is_equal(refusals_before + 1)
	assert_int(int(stats["consolidations"])).is_equal(successes_before)
	# Success path: the async frame-pump path owns the "consolidations" success counter
	# (a FORCED region, as above, deliberately never bumps it -- pre-Task-11 semantics).
	# The append spine auto-enqueues a region once its op list reaches kConsolidateAtOps
	# (192), so load it exactly the way test_consolidation.gd's async test does, then pump
	# to completion. First bake on a cold worker can outlive wait()'s 2 s deadline, so
	# poll like every async test in that suite.
	for i in range(192):
		_world.hooks().debug_apply_sphere_subtract(
				Vector3(24.0 + float(i % 4) * 0.25, 55.0, 24.0 + float(i / 4 % 4) * 0.25), 0.8)
	_world.hooks().debug_pump_consolidation_async()
	var completed := false
	for i in range(2000):
		_world.hooks().debug_pump_consolidation_async()
		stats = _world.hooks().debug_stream_stats()
		if int(stats["consolidations"]) > successes_before:
			completed = true
			break
		OS.delay_msec(5)
	assert_bool(completed).override_failure_message("async consolidation never completed").is_true()
	assert_int(int(stats["consolidation_refusals"])).is_equal(refusals_before + 1)
	assert_int(_world.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)

# Phase-4 contract smoke test (Task 14): compositor-callback admission refuses once
# shutdown_render_resources() has run, and re-admits after a lifecycle reopen + re-init --
# pinned at the orchestrator boundary (mirrors what test_render_shutdown.gd proves
# end-to-end). try_begin_render_callback/end_render_callback are ClassDB-bound as of this
# task so GDScript can exercise the guard directly.
func test_render_callback_admission_shuts_down_cleanly() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.use_local_device = true
	_world.physics_enabled = false
	add_child(_world)
	_world.ensure_initialized()
	assert_bool(_world.is_initialized()).is_true()
	assert_bool(_world.try_begin_render_callback()).is_true()
	_world.end_render_callback()
	_world.shutdown_render_resources()
	# Admission stays latched shut after shutdown...
	assert_bool(_world.try_begin_render_callback()).is_false()
	# ...until a fresh lifecycle entry runs VoxelWorld._ready()'s
	# voxel_compositor_callbacks_ready -> reopen_admission, exactly as a scene
	# re-instantiation would. Then re-init rebuilds the graph and admits again.
	remove_child(_world)
	_world.request_ready()
	add_child(_world)
	_world.ensure_initialized()
	assert_bool(_world.is_initialized()).is_true()
	assert_bool(_world.try_begin_render_callback()).is_true()
	_world.end_render_callback()

# Phase-5 contract smoke test (Task 15): after the LoD runtime settles, the debug facade's
# fade band -- one-line delegation into LodSystem::fade_band(), the single source of truth
# since the move out of VoxelWorld -- must equal a fresh read of the same band, and both
# endpoints must satisfy start < end. The M5 seam tests (test_lod_seam.gd) pin compositing
# behavior at exactly these two distances, so a band whose start >= end belongs to no
# field. Settle pattern from test_lod_build.gd/test_lod_pool.gd: poll debug_lod_tick until
# the walk requests nothing and nothing is in flight for a quiet streak.
const LOD_SETTLE_BUDGET := 2500
const LOD_QUIET_TICKS := 8
const LOD_POS := Vector3(400.0, 70.0, 400.0)
const LOD_FWD := Vector3(0, -0.2, -1)

func test_lod_fade_band_is_single_source_of_truth(timeout := 60000) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.use_local_device = true
	_world.physics_enabled = false
	_world.world_origin_bricks = Vector3i(0, -64, 0)
	_world.world_size_regions = Vector3i(8, 5, 8)
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	assert_bool(_world.hooks().debug_init_physics()).is_true()
	# Settle the far field exactly as the LoD suites do (requests_pending dips while a
	# batch lands, so convergence has to hold for a streak, not one frame).
	var settled := false
	var quiet := 0
	for i in range(LOD_SETTLE_BUDGET):
		_world.hooks().debug_lod_tick(LOD_POS, LOD_FWD)
		await get_tree().process_frame
		var stats: Dictionary = _world.hooks().debug_lod_stats()
		quiet = quiet + 1 if int(stats["requests_pending"]) == 0 \
				and int(stats["builds_in_flight"]) == 0 else 0
		if quiet >= LOD_QUIET_TICKS:
			settled = true
			break
	assert_bool(settled).override_failure_message(
		"the far field never converged; stats: %s"
			% _world.hooks().debug_lod_stats()).is_true()
	# One source of truth: two reads through the facade agree...
	var band: Vector2 = _world.hooks().debug_lod_fade_band()
	var again: Vector2 = _world.hooks().debug_lod_fade_band()
	assert_vector(band).override_failure_message(
		"the fade band moved between two reads after settle: %s vs %s" % [band, again]
	).is_equal(again)
	# ...and the invariant every seam assertion leans on holds.
	assert_float(band.x).override_failure_message(
		"fade start must be positive after settle: %s" % band).is_greater(0.0)
	assert_float(band.y).override_failure_message(
		"fade end must exceed fade start: %s" % band).is_greater(band.x)
