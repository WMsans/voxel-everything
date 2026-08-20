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
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 512
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, center: Vector3, frames := 6000) -> bool:
	var quiet := 0
	for i in range(frames):
		w.debug_physics_frame(center)
		var st: Dictionary = w.debug_physics_stats()
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
		var truth: Dictionary = w.debug_raycast(from, Vector3(0, -1, 0))
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
	var st: Dictionary = w.debug_physics_stats()
	assert_int(int(st["max_build_tris"])).is_less(int(st["max_chunk_tris"]))
	assert_int(int(st["max_build_tris"])).is_greater(0)

func test_octant_bodies_replace_atomically_and_report_diagnostics(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(60.0, 55.0, 60.0)
	assert_bool(settle(w, center)).is_true()
	var before: Dictionary = w.debug_physics_stats()
	# A chunk is represented by one body per populated octant, while `bodies` remains the
	# historical chunk count. More than one raw body proves that the split is live, not merely
	# a diagnostic rename of the old one-body path.
	assert_int(int(before["bodies_raw"])).is_greater(int(before["bodies"]))
	assert_int(int(before["bodies_raw"])).is_less_equal(int(before["bodies"]) * 8)

	var truth: Dictionary = w.debug_raycast(Vector3(center.x, 80.0, center.z), Vector3(0, -1, 0))
	assert_bool(bool(truth["hit"])).is_true()
	var chunk := chunk_for_point(truth["pos"])
	var old_info: Dictionary = w.debug_chunk_collider_info(chunk)
	assert_int(int(old_info["slot"])).is_greater_equal(0)
	var old_body: RID = w.debug_body_of_chunk(chunk)
	assert_bool(old_body.is_valid()).is_true()

	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var result: Dictionary = tool.apply_sphere_subtract(truth["pos"], 1.5)
	assert_array(result["rejected"]).is_empty()
	assert_bool(settle(w, center)).is_true()
	var new_info: Dictionary = w.debug_chunk_collider_info(chunk)
	assert_int(int(new_info["build_count"])).is_greater(int(old_info["build_count"]))
	# The replacement is committed only after every populated octant is built; diagnostics must
	# still find a real body even when octant zero is empty.
	var new_body: RID = w.debug_body_of_chunk(chunk)
	assert_bool(new_body.is_valid()).is_true()
	var after: Dictionary = w.debug_physics_stats()
	assert_int(int(after["bodies_raw"])).is_greater_equal(int(after["bodies"]))
