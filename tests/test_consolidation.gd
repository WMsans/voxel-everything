extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	w.hooks().debug_stream_region(Vector3i(0, 2, 0))
	return w

# The bake has to reproduce the field it replaces. debug_consolidate_diff bakes one region's
# bricks on the worker device, then compares every baked lattice sample against ve::eval_field
# with the same ops -- the same shape as debug_brick_diff, one level up.
func test_bake_reproduces_the_field(timeout := 60000) -> void:
	var w := make_world()
	for i in range(12):
		w.hooks().debug_apply_sphere_subtract(Vector3(24.0 + float(i) * 0.5, 51.5, 24.0), 1.2)
	var d: Dictionary = w.hooks().debug_consolidate_diff(Vector3i(0, 2, 0))
	assert_int(int(d["bricks"])).is_greater(0)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)
	assert_int(int(d.get("normal_count", 0))).is_equal(4913)
	assert_float(float(d.get("normal_min_length", 0.0))).is_greater(0.99)
	assert_float(float(d.get("normal_min_dot", 0.0))).is_greater(0.98)

# An overridden brick must read back through the FIELD, not just out of the pool: the whole
# point is that every consumer sees the same base.
func test_overridden_brick_reads_through_eval_field(timeout := 60000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	# The op list is gone; the crater must still be there.
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)
	var hit: Dictionary = w.hooks().debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	assert_float(float(hit["pos"].y)).is_less(51.4 - 1.0)

# A second bake must read the first bake as its base. This catches staging output that aliases
# a live override slot: the first crater would otherwise be silently restored or corrupted.
func test_reconsolidation_preserves_previous_bake(timeout := 120000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.hooks().debug_apply_sphere_subtract(Vector3(27.0, 51.4, 24.4), 1.5)
	var d: Dictionary = w.hooks().debug_consolidate_diff(Vector3i(0, 2, 0))
	assert_int(int(d["bricks"])).is_greater(0)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)
	assert_int(int(d.get("normal_count", 0))).is_equal(4913)
	assert_float(float(d.get("normal_min_dot", 0.0))).is_greater(0.98)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)
	var first: Dictionary = w.hooks().debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	var second: Dictionary = w.hooks().debug_raycast(Vector3(27.0, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(first["hit"]).is_true()
	assert_bool(second["hit"]).is_true()
	assert_float(float(first["pos"].y)).is_less(50.4)
	assert_float(float(second["pos"].y)).is_less(50.8)

func test_full_pool_refusal_preserves_edit_log_after_actual_exhaustion() -> void:
	var w := make_world()
	w.hooks().debug_stream_region(Vector3i(0, -2, 0))
	# The fill publishes through the streamed region's tenant slot.
	assert_bool(w.hooks().debug_fill_override_pool(Vector3i(0, -2, 0))).is_true()
	assert_int(w.hooks().debug_override_used()).is_equal(8192)
	w.hooks().debug_apply_sphere_subtract(Vector3(12.8, 51.2, 12.8), 2.0)
	var before := w.hooks().debug_region_op_count(Vector3i(0, 2, 0))
	assert_int(before).is_greater(0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	assert_int(w.hooks().debug_override_used()).is_equal(8192)
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(before)

func test_debug_fill_refuses_offscreen_region_without_touching_slot_zero() -> void:
	var w := make_world()
	w.hooks().debug_stream_frame(Vector3(1000.0, 1000.0, 1000.0))
	assert_int(w.hooks().debug_slot_of_region(Vector3i(0, 2, 0))).is_equal(-1)
	# (0, -2, 0) is 1700 m from the stream camera: non-resident, so the fill refuses.
	assert_bool(w.hooks().debug_fill_override_pool(Vector3i(0, -2, 0))).is_false()
	assert_int(w.hooks().debug_override_used()).is_equal(0)

func test_failed_consolidation_preserves_old_publication() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.hooks().debug_apply_sphere_subtract(Vector3(27.0, 51.4, 24.4), 1.5)
	w.hooks().debug_set_fail_consolidations(true)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	w.hooks().debug_set_fail_consolidations(false)
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(1)
	var old: Dictionary = w.hooks().debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(old["hit"]).is_true()
	assert_float(float(old["pos"].y)).is_less(50.4)

func test_restore_failure_rolls_back_cpu_and_both_gpu_pools() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.hooks().debug_apply_sphere_subtract(Vector3(27.0, 51.4, 24.4), 1.5)
	var used_before := w.hooks().debug_override_used()
	w.hooks().debug_set_fail_consolidate_uploads(true)
	w.hooks().debug_set_fail_restore_overrides(true)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	w.hooks().debug_set_fail_consolidate_uploads(false)
	assert_int(w.hooks().debug_override_used()).is_equal(used_before)
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(1)
	var old: Dictionary = w.hooks().debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(old["hit"]).is_true()
	assert_float(float(old["pos"].y)).is_less(50.4)
	w.hooks().debug_stream_region(Vector3i(0, 2, 0))
	var state: Dictionary = w.hooks().debug_override_render_state(Vector3i(30, 64, 30))
	assert_int(int(state["table_slot"])).override_failure_message("render state %s" % state).is_equal(int(state["cpu_slot"]))
	assert_bool(state["sdf_match"]).override_failure_message("render state %s" % state).is_true()
	assert_bool(state["mat_match"]).override_failure_message("render state %s" % state).is_true()
	var lod: Dictionary = w.hooks().debug_lod_diff(0, Vector3i(1, 4, 1))
	assert_int(int(lod["fine_max_diff"])).is_less_equal(1)
	assert_int(int(lod["reduced_max_diff"])).is_less_equal(1)

func test_teardown_releases_staged_override_slots_before_reinit() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	var used_before := w.hooks().debug_override_used()
	for i in range(192):
		w.hooks().debug_apply_sphere_subtract(Vector3(8.4 + float(i % 3) * 0.2, 51.4, 8.4), 1.5)
	w.hooks().debug_set_fail_consolidate_uploads(true)
	w.hooks().debug_pump_consolidation_async()
	var staged := false
	for i in range(1600):
		OS.delay_msec(5)
		w.hooks().debug_pump_consolidation_async()
		if w.hooks().debug_override_used() > used_before:
			staged = true
			break
	assert_bool(staged).override_failure_message("transaction never reached publication staging").is_true()
	w.hooks().debug_teardown_physics()
	assert_int(w.hooks().debug_override_used()).is_equal(used_before)
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_greater_equal(1)

func test_publication_failure_recovers_with_queued_worker_work(timeout := 120000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 55.0, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	for i in range(192):
		w.hooks().debug_apply_sphere_subtract(Vector3(20.0 + float(i % 4) * 0.25, 55.0,
				20.0 + float(i / 4 % 4) * 0.25), 0.8)

	# Hold the worker only after it has dequeued the publication. Jobs submitted from this
	# point are deterministically queued behind the publication rather than racing the worker's
	# extract -> mesh -> LoD -> consolidation -> publication priority order.
	w.hooks().debug_set_fail_consolidate_uploads(true)
	w.hooks().debug_set_fail_restore_overrides_always(true)
	w.hooks().debug_set_pause_override_publication(true)
	var publication_paused := false
	for i in range(1600):
		w.hooks().debug_pump_consolidation_async()
		if w.hooks().debug_override_publication_paused():
			publication_paused = true
			break
		OS.delay_msec(2)
	assert_bool(publication_paused).override_failure_message(
			"publication never reached the worker barrier").is_true()

	var mesh_chunks := [Vector3i(2, 8, 2), Vector3i(3, 8, 2)]
	assert_bool(w.hooks().debug_mesh_submit(mesh_chunks)).is_true()
	assert_bool(w.hooks().debug_extract_submit(73, Vector3i(30, 64, 30),
			Vector3i(30, 64, 30))).is_true()
	assert_bool(w.hooks().debug_lod_submit([[0, Vector3i(1, 4, 1)]])).is_true()

	var refusals_before: int = int(w.hooks().debug_stream_stats().get("consolidation_refusals", 0))
	w.hooks().debug_set_pause_override_publication(false)
	var refused := false
	for i in range(1600):
		w.hooks().debug_pump_consolidation_async()
		if int(w.hooks().debug_stream_stats().get("consolidation_refusals", 0)) > refusals_before:
			refused = true
			break
		OS.delay_msec(2)
	assert_bool(refused).override_failure_message("publication failure did not recover").is_true()

	# The failed publication must preserve its edit prefix and cancel every queued consumer with
	# an explicit failed result so each producer can release scheduler-facing in-flight state.
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(192)
	var mesh_results := w.hooks().debug_mesh_collect()
	var extract_results: Array = w.hooks().debug_extract_collect()
	var lod_results := w.hooks().debug_lod_collect()
	assert_int(mesh_results.size()).is_equal(2)
	for i in range(mesh_results.size()):
		assert_object(mesh_results[i]["chunk"]).is_equal(mesh_chunks[i])
		assert_bool(mesh_results[i]["failed"]).is_true()
	assert_int(extract_results.size()).is_equal(1)
	assert_int(int(extract_results[0]["id"])).is_equal(73)
	assert_int(int(extract_results[0]["kind"])).is_equal(0)
	assert_bool(extract_results[0]["failed"]).is_true()
	assert_int(lod_results.size()).is_equal(1)
	assert_vector(lod_results[0]["coord"]).is_equal(Vector3i(1, 4, 1))
	assert_bool(lod_results[0]["failed"]).is_true()

	# Clearing injection lets the requeued transaction complete on the rebuilt worker. This is
	# the recovery completion assertion: the captured prefix is published and then removed.
	w.hooks().debug_set_fail_consolidate_uploads(false)
	w.hooks().debug_set_fail_restore_overrides_always(false)
	var consolidations_before_retry: int = int(w.hooks().debug_stream_stats().get("consolidations", 0))
	var recovered := false
	for i in range(2000):
		w.hooks().debug_pump_consolidation_async()
		if int(w.hooks().debug_stream_stats().get("consolidations", 0)) > consolidations_before_retry:
			recovered = true
			break
		OS.delay_msec(5)
	assert_bool(recovered).override_failure_message("worker recovery did not complete").is_true()
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)

func test_mesh_consumer_replays_consolidated_override_after_worker_reinit(timeout := 120000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	var before: Dictionary = w.hooks().debug_mesh_diff(Vector3i(3, 8, 3))
	assert_int(int(before["op_count"])).is_equal(0)
	assert_int(int(before["lattice_diff_over_one"])).is_equal(0)
	var lod_before: Dictionary = w.hooks().debug_lod_diff(0, Vector3i(1, 4, 1))
	assert_int(int(lod_before["fine_max_diff"])).is_less_equal(1)
	assert_int(int(lod_before["reduced_max_diff"])).is_less_equal(1)
	w.hooks().debug_teardown_physics()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var after: Dictionary = w.hooks().debug_mesh_diff(Vector3i(3, 8, 3))
	assert_int(int(after["op_count"])).is_equal(0)
	assert_int(int(after["lattice_diff_over_one"])).is_equal(0)
	assert_int(int(after["cells_only_cpu"])).is_equal(0)
	assert_int(int(after["cells_only_gpu"])).is_equal(0)
	var lod_after: Dictionary = w.hooks().debug_lod_diff(0, Vector3i(1, 4, 1))
	assert_int(int(lod_after["fine_max_diff"])).is_less_equal(1)
	assert_int(int(lod_after["reduced_max_diff"])).is_less_equal(1)

# The wall M2 and M4 both left standing: op 257 in one region used to be rejected and logged.
# It must now land, because the list consolidates itself out of the way first.
func test_three_hundred_edits_in_one_region_all_land(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(24.0, 51.5, 24.0)
	for i in range(300):
		var a := float(i) * 0.21
		w.hooks().debug_apply_sphere_subtract(center + Vector3(cos(a) * 2.0, sin(a * 0.7) * 1.5, sin(a) * 2.0), 0.9)
		w.hooks().debug_pump_consolidation() # what _process does once a frame
	var st: Dictionary = w.hooks().debug_stream_stats()
	assert_int(int(st["overflow_ever"])).is_equal(0)
	assert_int(int(st["edit_rejections"])).is_equal(0)
	assert_int(int(st["consolidations"])).is_greater(0)
	assert_int(int(st["override_bricks"])).is_greater(0)
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_less(192)
	var final_surface: Dictionary = w.hooks().debug_raycast(center + Vector3(0, 20, 0), Vector3(0, -1, 0))
	assert_bool(final_surface["hit"]).is_true()

# Consolidation must not change what the world looks like. The oracle is the raycast the
# edit tool aims with -- the same field the renderer marches.
func test_consolidation_preserves_the_surface(timeout := 120000) -> void:
	var w := make_world()
	for i in range(40):
		w.hooks().debug_apply_sphere_subtract(Vector3(24.0 + float(i % 7) * 0.4, 51.5, 24.0), 1.0)
	var before: Array = []
	for i in range(16):
		var p := Vector3(22.0 + float(i) * 0.3, 70.0, 24.0)
		before.append(w.hooks().debug_raycast(p, Vector3(0, -1, 0)))
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	for i in range(16):
		var p := Vector3(22.0 + float(i) * 0.3, 70.0, 24.0)
		var after: Dictionary = w.hooks().debug_raycast(p, Vector3(0, -1, 0))
		var was: Dictionary = before[i]
		assert_bool(after["hit"]).is_equal(was["hit"])
		if bool(was["hit"]):
			assert_float(float(after["pos"].y)).is_equal_approx(float(was["pos"].y), 0.06)

# A full pool must leave the world exactly as it was.
func test_automatic_consolidation_refuses_without_resident_slot() -> void:
	var w := make_world()
	var region := Vector3i(1, 2, 1)
	w.hooks().debug_apply_sphere_subtract(Vector3(56.4, 51.4, 56.4), 2.0)
	var ops_before := w.hooks().debug_region_op_count(region)
	assert_int(w.hooks().debug_slot_of_region(region)).is_equal(-1)
	assert_bool(w.hooks().debug_consolidate_region(region)).is_false()
	assert_int(w.hooks().debug_region_op_count(region)).is_equal(ops_before)

func test_async_consolidation_keeps_edits_appended_during_bake(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(24.0, 53.0, 24.0)
	for i in range(192):
		var a := float(i) * 0.21
		w.hooks().debug_apply_sphere_subtract(center + Vector3(cos(a) * 2.0, 0.0, sin(a) * 2.0), 0.9)
	w.hooks().debug_pump_consolidation_async()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.0, 53.0, 24.0), 0.9)
	w.hooks().debug_wait_consolidation()
	var async_stats: Dictionary = w.hooks().debug_stream_stats()
	assert_int(int(async_stats["consolidations"])).is_equal(1)
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(1)
	assert_int(int(async_stats["edit_rejections"])).is_equal(0)
	var surface: Dictionary = w.hooks().debug_raycast(Vector3(24.0, 70.0, 24.0), Vector3(0, -1, 0))
	assert_bool(surface["hit"]).is_true()

func test_invalid_override_capacity_fails_softly() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.max_override_bricks = -1
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	assert_int(w.get_max_override_bricks()).is_equal(0)
	assert_int(int(w.hooks().debug_stream_stats().get("override_capacity", 0))).is_equal(0)

func test_a_full_pool_refuses_and_changes_nothing(timeout := 60000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.max_override_bricks = 4
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	assert_int(int(w.hooks().debug_stream_stats()["override_capacity"])).is_equal(4)
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	var ops_before: int = w.hooks().debug_region_op_count(Vector3i(0, 2, 0))
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(ops_before)
	assert_int(int(w.hooks().debug_stream_stats()["consolidation_refusals"])).is_greater(0)

func test_reconsolidation_with_volume_add_captures_both_sources(timeout := 120000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	# Create a small volume and paste it - dim 64 to match worker pool
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	var dim := 64
	var n := dim*dim*dim
	sdf.resize(n)
	mat.resize(n)
	for i in range(n):
		sdf[i] = 255
		mat[i] = 0
	for z in range(dim): for y in range(dim): for x in range(dim):
		var p := Vector3(x, y, z) * 0.05 - Vector3(1.575, 1.575, 1.575)
		var d: float = p.length() - 0.5
		var idx := x + y*dim + z*dim*dim
		if d <= 0.0:
			sdf[idx] = 100
			mat[idx] = 2
	w.hooks().debug_store_volume(1, sdf, mat, dim)
	w.hooks().debug_apply_volume_add(1, Vector3(10.01, 60.01, 10.01), 0.05, dim)
	w.hooks().debug_apply_sphere_subtract(Vector3(20.01, 60.01, 20.01), 1.0)
	var d: Dictionary = w.hooks().debug_consolidate_diff(Vector3i(0, 2, 0))
	assert_int(int(d["bricks"])).is_greater(0)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)
	assert_int(int(d.get("normal_count", 0))).is_equal(4913)
	assert_float(float(d.get("normal_min_length", 0.0))).is_greater(0.99)
	assert_float(float(d.get("normal_min_dot", 0.0))).is_greater(0.98)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)

func test_render_pool_replays_consolidated_override_after_atlas_reinit() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.hooks().debug_teardown_atlas()
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	w.hooks().debug_stream_region(Vector3i(0, 2, 0))
	var rslot := w.hooks().debug_region_map_entry(Vector3i(0, 2, 0))
	assert_int(rslot).is_greater_equal(0)
	assert_int(w.hooks().debug_override_region_table(rslot)).is_greater_equal(0)
	var state: Dictionary = w.hooks().debug_override_render_state(Vector3i(30, 64, 30))
	assert_int(int(state["table"])).is_greater_equal(0)
	assert_int(int(state["table_slot"])).override_failure_message("render state %s" % state).is_equal(int(state["cpu_slot"]))
	assert_bool(state["sdf_match"]).override_failure_message("render state %s" % state).is_true()
	assert_bool(state["mat_match"]).override_failure_message("render state %s" % state).is_true()
