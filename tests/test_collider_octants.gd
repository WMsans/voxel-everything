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
func test_no_single_build_carries_the_whole_chunk(timeout := 120000) -> void:
	var w := make_world()
	assert_bool(settle(w, Vector3(60.0, 55.0, 60.0))).is_true()
	var st: Dictionary = w.debug_physics_stats()
	assert_int(int(st["max_build_tris"])).is_less(int(st["max_chunk_tris"]))
	assert_int(int(st["max_build_tris"])).is_greater(0)
