extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(shape_builds_per_frame := 4) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 512
	w.shape_builds_per_frame = shape_builds_per_frame
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, center: Vector3, frames := 6000) -> bool:
	var quiet := 0
	for i in range(frames):
		w.hooks().debug_physics_frame(center)
		var st: Dictionary = w.hooks().debug_physics_stats()
		quiet = quiet + 1 if st["chunks_pending"] == 0 and st["queued"] == 0 else 0
		if quiet >= 4:
			return true
	return false

# The ground must still hold the player up. This is the same oracle test_collider_stream.gd
# uses: the physics ray and ve::raycast agree to a few centimetres, whether the chunk is one
# body or eight.
func test_split_colliders_still_match_the_field(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(60.0, 55.0, 60.0)
	assert_bool(settle(w, center)).is_true()
	for i in range(8):
		var x := 55.0 + float(i)
		var from := Vector3(x, 70.0, 60.0)
		var hit := get_tree().root.world_3d.direct_space_state.intersect_ray(
				PhysicsRayQueryParameters3D.create(from, from + Vector3(0, -30, 0)))
		var truth: Dictionary = w.hooks().debug_raycast(from, Vector3(0, -1, 0))
		assert_bool(hit.is_empty()).is_false()
		if not hit.is_empty() and bool(truth["hit"]):
			assert_float(float(hit["position"].y)).is_equal_approx(float(truth["pos"].y), 0.15)

# The point of the split: no single build is the whole chunk any more.
func chunk_for_point(point: Vector3) -> Vector3i:
	return Vector3i(int(floor(point.x / 6.4)), int(floor(point.y / 6.4)),
			int(floor(point.z / 6.4)))

func test_no_single_build_carries_the_whole_chunk(timeout := 120000) -> void:
	var w := make_world()
	assert_bool(settle(w, Vector3(60.0, 55.0, 60.0))).is_true()
	var st: Dictionary = w.hooks().debug_physics_stats()
	assert_int(int(st["max_build_tris"])).is_less(int(st["max_chunk_tris"]))
	assert_int(int(st["max_build_tris"])).is_greater(0)

func test_throttled_remesh_keeps_old_collider_until_commit(timeout := 120000) -> void:
	var w := make_world(1)
	var center := Vector3(60.0, 55.0, 60.0)
	assert_bool(settle(w, center)).is_true()
	var before: Dictionary = w.hooks().debug_physics_stats()
	var truth: Dictionary = w.hooks().debug_raycast(Vector3(center.x, 80.0, center.z), Vector3(0, -1, 0))
	assert_bool(bool(truth["hit"])).is_true()
	var chunk := chunk_for_point(truth["pos"])
	var old_body: RID = w.hooks().debug_body_of_chunk(chunk)
	assert_bool(old_body.is_valid()).is_true()
	var old_raw := int(before["bodies_raw"])
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var result: Dictionary = tool.apply_sphere_subtract(truth["pos"], 1.5)
	assert_array(result["rejected"]).is_empty()

	var saw_staging := false
	for i in range(1200):
		w.hooks().debug_physics_frame(center)
		var st: Dictionary = w.hooks().debug_physics_stats()
		if int(st["chunks_pending"]) > 0:
			saw_staging = true
			# One octant is allowed to build per frame, but no replacement may expose a partial
			# collider: the old body remains live until the staged octants commit as a set.
			assert_int(int(st["builds"])).is_less_equal(1)
			assert_int(int(st["bodies_raw"])).is_equal(old_raw)
			assert_bool(w.hooks().debug_body_of_chunk(chunk).is_valid()).is_true()
			var live_hit := get_tree().root.world_3d.direct_space_state.intersect_ray(
					PhysicsRayQueryParameters3D.create(
							Vector3(center.x, 80.0, center.z), Vector3(center.x, 20.0, center.z)))
			assert_bool(live_hit.is_empty()).is_false()
			break
	assert_bool(saw_staging).override_failure_message(
			"the edit completed without exposing a throttled staging window").is_true()
	assert_bool(settle(w, center)).is_true()
	var after: Dictionary = w.hooks().debug_physics_stats()
	assert_int(int(after["bodies_raw"])).is_greater_equal(int(after["bodies"]))
	assert_bool(w.hooks().debug_body_of_chunk(chunk).is_valid()).is_true()

func test_octant_bodies_replace_atomically_and_report_diagnostics(timeout := 120000) -> void:
	var w := make_world(1)
	# This exact chunk contains the deterministic cave and terrain surface in several, but not
	# all, centroid octants. Empty octants must remain empty raw slots: diagnostic proof must
	# not manufacture shape-less PhysicsServer bodies.
	var center := Vector3(35.0, 55.0, 35.0)
	assert_bool(settle(w, center)).is_true()
	var before: Dictionary = w.hooks().debug_physics_stats()
	assert_int(int(before["bodies_raw"])).is_greater(int(before["bodies"]))
	assert_int(int(before["bodies_raw"])).is_less(int(before["bodies"]) * 8)

	var truth: Dictionary = w.hooks().debug_raycast(Vector3(center.x, 80.0, center.z), Vector3(0, -1, 0))
	assert_bool(bool(truth["hit"])).is_true()
	var chunk := chunk_for_point(truth["pos"])
	var old_diag: Dictionary = w.hooks().debug_chunk_collider_octants(chunk)
	assert_int(int(old_diag["slot"])).is_greater_equal(0)
	assert_bool(bool(old_diag["staged"])).is_false()
	assert_int(int(old_diag["octants"].size())).is_equal(8)
	var old_ids := PackedInt64Array()
	var old_populated := 0
	for i in range(8):
		var octant: Dictionary = old_diag["octants"][i]
		assert_int(int(octant["octant"])).is_equal(i)
		assert_int(int(octant["slot"])).is_equal(int(old_diag["slot"]) * 8 + i)
		var rid_id := int(octant["rid_id"])
		assert_bool(bool(octant["valid"])).is_equal(rid_id > 0)
		old_ids.append(rid_id)
		if rid_id > 0:
			old_populated += 1
	assert_int(old_populated).is_greater(1)
	assert_int(old_populated).is_less(8)

	var old_build_count := int(old_diag["build_count"])
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var result: Dictionary = tool.apply_sphere_subtract(truth["pos"], 1.5)
	assert_array(result["rejected"]).is_empty()

	var saw_staging := false
	var saw_replacement_build := false
	var saw_commit := false
	var max_staged_built_octants := 0
	for i in range(1600):
		w.hooks().debug_physics_frame(center)
		var diag: Dictionary = w.hooks().debug_chunk_collider_octants(chunk)
		if bool(diag["staged"]):
			saw_staging = true
			# At least one replacement shape has built, but every live raw slot -- populated or
			# empty -- must still be the exact old sparse set until the transaction commits.
			assert_int(int(diag["staged_built_octants"])).is_greater(0)
			max_staged_built_octants = maxi(max_staged_built_octants,
					int(diag["staged_built_octants"]))
			saw_replacement_build = true
			for octant in diag["octants"]:
				var live: Dictionary = octant
				var index := int(live["octant"])
				assert_int(int(live["rid_id"])).is_equal(int(old_ids[index]))
				assert_bool(bool(live["valid"])).is_equal(int(old_ids[index]) > 0)
		else:
			if saw_staging and int(diag["build_count"]) > old_build_count:
				saw_commit = true
				break
	assert_bool(saw_staging).override_failure_message(
			"the edit completed without exposing a staged octant replacement").is_true()
	assert_bool(saw_replacement_build).override_failure_message(
			"no replacement octant build was observed while the replacement was staged").is_true()
	assert_bool(saw_commit).override_failure_message(
			"the staged replacement did not commit after all octants were built").is_true()

	assert_bool(settle(w, center)).is_true()
	var new_diag: Dictionary = w.hooks().debug_chunk_collider_octants(chunk)
	assert_bool(bool(new_diag["staged"])).is_false()
	assert_int(int(new_diag["build_count"])).is_greater(old_build_count)
	assert_int(int(new_diag["octants"].size())).is_equal(8)
	var new_ids := PackedInt64Array()
	var new_populated := 0
	for i in range(8):
		var octant: Dictionary = new_diag["octants"][i]
		assert_int(int(octant["octant"])).is_equal(i)
		var rid_id := int(octant["rid_id"])
		assert_bool(bool(octant["valid"])).is_equal(rid_id > 0)
		new_ids.append(rid_id)
		if rid_id > 0:
			new_populated += 1
			assert_bool(old_ids.has(rid_id)).override_failure_message(
					"a replacement octant retained an old body RID").is_false()
	assert_int(new_populated).is_greater(1)
	assert_int(new_populated).is_less(8)
	print("COLLIDER_OCTANT_PROOF chunk=", chunk, " slot=", old_diag["slot"],
			" old_rids=", old_ids, " new_rids=", new_ids,
			" max_staged_built_octants=", max_staged_built_octants)
