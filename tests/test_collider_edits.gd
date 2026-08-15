extends GdUnitTestSuite

# Spec section 6: "Edit -> remesh -> collidable again in 1-2 frames." The collider is
# rebuilt from the same op list the renderer uses, so what you shot through is what you fall
# through.

const CENTER := Vector3(60.0, 55.0, 60.0)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 64
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, center: Vector3, frames := 400) -> void:
	var quiet := 0
	for i in range(frames):
		quiet = quiet + 1 if w.debug_physics_frame(center) == 0 else 0
		if quiet >= 4:
			return

func ray(from: Vector3, to: Vector3) -> Dictionary:
	var state := get_tree().root.world_3d.direct_space_state
	return state.intersect_ray(PhysicsRayQueryParameters3D.create(from, to))

func test_carving_makes_the_ground_fall_away(timeout := 90000) -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	settle(w, CENTER)
	await get_tree().physics_frame

	var from := Vector3(CENTER.x, 80.0, CENTER.z)
	var to := Vector3(CENTER.x, 20.0, CENTER.z)
	var before := ray(from, to)
	assert_bool(before.is_empty()).is_false()
	var surface: float = before["position"].y

	var r: Dictionary = tool.apply_sphere_subtract(Vector3(CENTER.x, surface + 0.5, CENTER.z), 3.0)
	assert_array(r["rejected"]).is_empty()
	# A blast dirties a handful of chunks; at two jobs a frame the one under the player
	# rebuilds first, so a few frames is generous for "1-2".
	for i in range(30):
		w.debug_physics_frame(CENTER)
	await get_tree().physics_frame

	var after := ray(from, to)
	assert_bool(after.is_empty()).override_failure_message(
		"the crater floor has no collider at all").is_false()
	assert_float(after["position"].y).override_failure_message(
		"collision did not follow the crater: %f vs %f" % [after["position"].y, surface]
		).is_less(surface - 1.0)
	# ...and it still agrees with the field the renderer draws.
	var oracle: Dictionary = w.debug_raycast(from, Vector3(0, -1, 0))
	assert_float(after["position"].y).is_equal_approx(oracle["pos"].y, 0.15)

func test_filling_makes_new_ground_collidable(timeout := 90000) -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	settle(w, CENTER)

	# Well above the terrain (which tops out at 51.2 + 10) and still inside the world, whose
	# y span here is [-51.2, 76.8): nothing to stand on, and the chunk's probe has said so.
	var from := Vector3(CENTER.x, 78.0, CENTER.z)
	var to := Vector3(CENTER.x, 66.0, CENTER.z)
	assert_bool(ray(from, to).is_empty()).is_true()

	var r: Dictionary = tool.apply_sphere_add(Vector3(CENTER.x, 70.0, CENTER.z), 4.0, 4)
	assert_array(r["rejected"]).is_empty()
	settle(w, CENTER)
	await get_tree().physics_frame

	var hit := ray(from, to)
	assert_bool(hit.is_empty()).override_failure_message(
		"the added blob never became collidable — the cached empty verdict was not cleared"
		).is_false()
	assert_float(hit["position"].y).is_equal_approx(74.0, 0.3)

func test_a_paint_edit_rebuilds_nothing_it_does_not_have_to(timeout := 90000) -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	settle(w, CENTER)
	var hit: Dictionary = w.debug_raycast(Vector3(CENTER.x, 80.0, CENTER.z), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()

	# Paint changes no SDF, but it is still an op in the region's list, so the chunks it
	# touches are re-meshed once. The point of the test is that it CONVERGES — a dirty mark
	# that never clears would keep the mesher busy for ever.
	tool.apply_sphere_paint(hit["pos"], 3.0, 1)
	settle(w, CENTER)
	for i in range(20):
		assert_int(w.debug_physics_frame(CENTER)).is_equal(0)
