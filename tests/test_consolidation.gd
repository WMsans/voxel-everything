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
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	w.debug_stream_region(Vector3i(0, 2, 0))
	return w

# The bake has to reproduce the field it replaces. debug_consolidate_diff bakes one region's
# bricks on the worker device, then compares every baked lattice sample against ve::eval_field
# with the same ops -- the same shape as debug_brick_diff, one level up.
func test_bake_reproduces_the_field(timeout := 60000) -> void:
	var w := make_world()
	for i in range(12):
		w.debug_apply_sphere_subtract(Vector3(24.0 + float(i) * 0.5, 51.5, 24.0), 1.2)
	var d: Dictionary = w.debug_consolidate_diff(Vector3i(0, 2, 0))
	assert_int(int(d["bricks"])).is_greater(0)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)

# An overridden brick must read back through the FIELD, not just out of the pool: the whole
# point is that every consumer sees the same base.
func test_overridden_brick_reads_through_eval_field(timeout := 60000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	# The op list is gone; the crater must still be there.
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)
	var hit: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	assert_float(float(hit["pos"].y)).is_less(51.4 - 1.0)

# A second bake must read the first bake as its base. This catches staging output that aliases
# a live override slot: the first crater would otherwise be silently restored or corrupted.
func test_reconsolidation_preserves_previous_bake(timeout := 120000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.debug_apply_sphere_subtract(Vector3(27.0, 51.4, 24.4), 1.5)
	var d: Dictionary = w.debug_consolidate_diff(Vector3i(0, 2, 0))
	assert_int(int(d["bricks"])).is_greater(0)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(0)
	var first: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	var second: Dictionary = w.debug_raycast(Vector3(27.0, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(first["hit"]).is_true()
	assert_bool(second["hit"]).is_true()
	assert_float(float(first["pos"].y)).is_less(50.4)
	assert_float(float(second["pos"].y)).is_less(50.8)

func test_full_pool_refusal_preserves_edit_log_after_actual_exhaustion() -> void:
	var w := make_world()
	assert_bool(w.debug_fill_override_pool()).is_true()
	assert_int(w.debug_override_used()).is_equal(8192)
	w.debug_apply_sphere_subtract(Vector3(12.8, 51.2, 12.8), 2.0)
	var before := w.debug_region_op_count(Vector3i(0, 2, 0))
	assert_int(before).is_greater(0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	assert_int(w.debug_override_used()).is_equal(8192)
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(before)

func test_failed_consolidation_preserves_old_publication() -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.debug_apply_sphere_subtract(Vector3(27.0, 51.4, 24.4), 1.5)
	w.debug_set_fail_consolidations(true)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	w.debug_set_fail_consolidations(false)
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(1)
	var old: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(old["hit"]).is_true()
	assert_float(float(old["pos"].y)).is_less(50.4)

func test_restore_failure_rolls_back_cpu_and_both_gpu_pools() -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.debug_apply_sphere_subtract(Vector3(27.0, 51.4, 24.4), 1.5)
	var used_before := w.debug_override_used()
	w.debug_set_fail_consolidate_uploads(true)
	w.debug_set_fail_restore_overrides(true)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	w.debug_set_fail_consolidate_uploads(false)
	assert_int(w.debug_override_used()).is_equal(used_before)
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(1)
	var old: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(old["hit"]).is_true()
	assert_float(float(old["pos"].y)).is_less(50.4)
	w.debug_stream_region(Vector3i(0, 2, 0))
	var state: Dictionary = w.debug_override_render_state(Vector3i(30, 64, 30))
	assert_int(int(state["table_slot"])).override_failure_message("render state %s" % state).is_equal(int(state["cpu_slot"]))
	assert_bool(state["sdf_match"]).override_failure_message("render state %s" % state).is_true()
	assert_bool(state["mat_match"]).override_failure_message("render state %s" % state).is_true()
	var lod: Dictionary = w.debug_lod_diff(0, Vector3i(1, 4, 1))
	assert_int(int(lod["fine_max_diff"])).is_less_equal(1)
	assert_int(int(lod["reduced_max_diff"])).is_less_equal(1)

func test_mesh_consumer_replays_consolidated_override_after_worker_reinit(timeout := 120000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	var before: Dictionary = w.debug_mesh_diff(Vector3i(3, 8, 3))
	assert_int(int(before["op_count"])).is_equal(0)
	assert_int(int(before["lattice_diff_over_one"])).is_equal(0)
	var lod_before: Dictionary = w.debug_lod_diff(0, Vector3i(1, 4, 1))
	assert_int(int(lod_before["fine_max_diff"])).is_less_equal(1)
	assert_int(int(lod_before["reduced_max_diff"])).is_less_equal(1)
	w.debug_teardown_physics()
	assert_bool(w.debug_init_physics()).is_true()
	var after: Dictionary = w.debug_mesh_diff(Vector3i(3, 8, 3))
	assert_int(int(after["op_count"])).is_equal(0)
	assert_int(int(after["lattice_diff_over_one"])).is_equal(0)
	assert_int(int(after["cells_only_cpu"])).is_equal(0)
	assert_int(int(after["cells_only_gpu"])).is_equal(0)
	var lod_after: Dictionary = w.debug_lod_diff(0, Vector3i(1, 4, 1))
	assert_int(int(lod_after["fine_max_diff"])).is_less_equal(1)
	assert_int(int(lod_after["reduced_max_diff"])).is_less_equal(1)

# The wall M2 and M4 both left standing: op 257 in one region used to be rejected and logged.
# It must now land, because the list consolidates itself out of the way first.
func test_three_hundred_edits_in_one_region_all_land(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(24.0, 51.5, 24.0)
	for i in range(300):
		var a := float(i) * 0.21
		w.debug_apply_sphere_subtract(center + Vector3(cos(a) * 2.0, sin(a * 0.7) * 1.5, sin(a) * 2.0), 0.9)
		w.debug_pump_consolidation() # what _process does once a frame
	var st: Dictionary = w.debug_stream_stats()
	assert_int(int(st["overflow_ever"])).is_equal(0)
	assert_int(int(st["edit_rejections"])).is_equal(0)
	assert_int(int(st["consolidations"])).is_greater(0)
	assert_int(int(st["override_bricks"])).is_greater(0)
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_less(192)
	var final_surface: Dictionary = w.debug_raycast(center + Vector3(0, 20, 0), Vector3(0, -1, 0))
	assert_bool(final_surface["hit"]).is_true()

# Consolidation must not change what the world looks like. The oracle is the raycast the
# edit tool aims with -- the same field the renderer marches.
func test_consolidation_preserves_the_surface(timeout := 120000) -> void:
	var w := make_world()
	for i in range(40):
		w.debug_apply_sphere_subtract(Vector3(24.0 + float(i % 7) * 0.4, 51.5, 24.0), 1.0)
	var before: Array = []
	for i in range(16):
		var p := Vector3(22.0 + float(i) * 0.3, 70.0, 24.0)
		before.append(w.debug_raycast(p, Vector3(0, -1, 0)))
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	for i in range(16):
		var p := Vector3(22.0 + float(i) * 0.3, 70.0, 24.0)
		var after: Dictionary = w.debug_raycast(p, Vector3(0, -1, 0))
		var was: Dictionary = before[i]
		assert_bool(after["hit"]).is_equal(was["hit"])
		if bool(was["hit"]):
			assert_float(float(after["pos"].y)).is_equal_approx(float(was["pos"].y), 0.06)

# A full pool must leave the world exactly as it was.
func test_automatic_consolidation_refuses_without_resident_slot() -> void:
	var w := make_world()
	var region := Vector3i(1, 2, 1)
	w.debug_apply_sphere_subtract(Vector3(56.4, 51.4, 56.4), 2.0)
	var ops_before := w.debug_region_op_count(region)
	assert_int(w.debug_slot_of_region(region)).is_equal(-1)
	assert_bool(w.debug_consolidate_region(region)).is_false()
	assert_int(w.debug_region_op_count(region)).is_equal(ops_before)

func test_async_consolidation_keeps_edits_appended_during_bake(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(24.0, 53.0, 24.0)
	for i in range(192):
		var a := float(i) * 0.21
		w.debug_apply_sphere_subtract(center + Vector3(cos(a) * 2.0, 0.0, sin(a) * 2.0), 0.9)
	w.debug_pump_consolidation_async()
	w.debug_apply_sphere_subtract(Vector3(24.0, 53.0, 24.0), 0.9)
	w.debug_wait_consolidation()
	var async_stats: Dictionary = w.debug_stream_stats()
	assert_int(int(async_stats["consolidations"])).is_equal(1)
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(1)
	assert_int(int(async_stats["edit_rejections"])).is_equal(0)
	var surface: Dictionary = w.debug_raycast(Vector3(24.0, 70.0, 24.0), Vector3(0, -1, 0))
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
	assert_int(int(w.debug_stream_stats().get("override_capacity", 0))).is_equal(0)

func test_a_full_pool_refuses_and_changes_nothing(timeout := 60000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.max_override_bricks = 4
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	assert_int(int(w.debug_stream_stats()["override_capacity"])).is_equal(4)
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	var ops_before: int = w.debug_region_op_count(Vector3i(0, 2, 0))
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	assert_int(w.debug_region_op_count(Vector3i(0, 2, 0))).is_equal(ops_before)
	assert_int(int(w.debug_stream_stats()["consolidation_refusals"])).is_greater(0)

func test_render_pool_replays_consolidated_override_after_atlas_reinit() -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.debug_teardown_atlas()
	assert_bool(w.debug_init_atlas()).is_true()
	w.debug_stream_region(Vector3i(0, 2, 0))
	var rslot := w.debug_region_map_entry(Vector3i(0, 2, 0))
	assert_int(rslot).is_greater_equal(0)
	assert_int(w.debug_override_region_table(rslot)).is_greater_equal(0)
	var state: Dictionary = w.debug_override_render_state(Vector3i(30, 64, 30))
	assert_int(int(state["table"])).is_greater_equal(0)
	assert_int(int(state["table_slot"])).override_failure_message("render state %s" % state).is_equal(int(state["cpu_slot"]))
	assert_bool(state["sdf_match"]).override_failure_message("render state %s" % state).is_true()
	assert_bool(state["mat_match"]).override_failure_message("render state %s" % state).is_true()
