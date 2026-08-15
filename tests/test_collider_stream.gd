extends GdUnitTestSuite

# Collision streaming (spec section 6): dual-contoured chunks become Jolt concave shapes on
# server-created static bodies, in a ball around the player, with no scene-tree nodes.
#
# The physics ray is checked against ve::raycast on the analytic field — the same oracle
# test_edit_pipeline.gd uses for the renderer. If the two agree to a few centimetres, the
# collision the player walks on IS the terrain they can see.

const CENTER := Vector3(60.0, 55.0, 60.0)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false        # no auto tick: these tests step it by hand
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.physics_radius_m = 25.0        # a handful of chunks, not the shipping 160
	w.max_collider_chunks = 64
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

# Settled means several consecutive frames with nothing to do: one quiet frame is only the
# gap between submitting a batch and collecting it.
func settle(w: VoxelWorld, center: Vector3, frames := 400) -> void:
	var quiet := 0
	for i in range(frames):
		quiet = quiet + 1 if w.debug_physics_frame(center) == 0 else 0
		if quiet >= 4:
			return

func ray(from: Vector3, to: Vector3) -> Dictionary:
	var state := get_tree().root.world_3d.direct_space_state
	return state.intersect_ray(PhysicsRayQueryParameters3D.create(from, to))

func test_a_server_built_concave_shape_is_hit_by_a_ray(timeout := 20000) -> void:
	# Pins the shape_set_data contract the streamer depends on. Godot's own
	# ConcavePolygonShape3D sends {"faces": PackedVector3Array, "backface_collision": bool},
	# and the winding decides which side collides — exactly the two things that would make
	# the streamer silently produce nothing. Jolt's front-face convention is opposite to
	# Godot's right-hand-rule cross product, so this test uses the order Jolt treats as
	# facing up; ColliderStreamer performs the same swap when building chunk shapes.
	var shape := PhysicsServer3D.concave_polygon_shape_create()
	var faces := PackedVector3Array([
		Vector3(-1, 0, -1), Vector3(1, 0, 1), Vector3(-1, 0, 1),
		Vector3(-1, 0, -1), Vector3(1, 0, -1), Vector3(1, 0, 1)])
	PhysicsServer3D.shape_set_data(shape, {"faces": faces, "backface_collision": false})
	var body := PhysicsServer3D.body_create()
	PhysicsServer3D.body_set_mode(body, PhysicsServer3D.BODY_MODE_STATIC)
	PhysicsServer3D.body_add_shape(body, shape)
	PhysicsServer3D.body_set_space(body, get_tree().root.world_3d.space)
	await get_tree().physics_frame
	var hit := ray(Vector3(0, 5, 0), Vector3(0, -5, 0))
	assert_bool(hit.is_empty()).is_false()
	assert_float(hit["position"].y).is_equal_approx(0.0, 0.01)
	PhysicsServer3D.free_rid(body)
	PhysicsServer3D.free_rid(shape)

func test_colliders_appear_around_the_player(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var st: Dictionary = w.debug_physics_stats()
	assert_int(st["chunks_resident"]).is_greater(3)
	assert_int(st["chunks_pending"]).is_equal(0)
	assert_int(st["bodies"]).is_greater(3)
	assert_int(st["failures"]).is_equal(0)
	# Every resident chunk is inside the radius, so the pool never filled up here.
	assert_int(st["chunks_resident"]).is_less_equal(64)
	w.free()

func test_a_physics_ray_lands_on_the_analytic_surface(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	await get_tree().physics_frame

	var oracle: Dictionary = w.debug_raycast(Vector3(CENTER.x, 80.0, CENTER.z), Vector3(0, -1, 0))
	assert_bool(oracle["hit"]).is_true()
	var hit := ray(Vector3(CENTER.x, 80.0, CENTER.z), Vector3(CENTER.x, 20.0, CENTER.z))
	assert_bool(hit.is_empty()).override_failure_message(
		"the physics ray found no collider under the player").is_false()
	# 0.1 m cells plus the mesher's own error: a few centimetres, never a different surface.
	assert_float(hit["position"].y).is_equal_approx(oracle["pos"].y, 0.15)
	# The surface faces up, or a character would fall through it.
	assert_float(hit["normal"].y).is_greater(0.3)
	w.free()

func test_walking_away_releases_the_far_colliders(timeout := 90000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var before: Dictionary = w.debug_physics_stats()
	assert_int(before["bodies"]).is_greater(3)

	var far := CENTER + Vector3(80.0, 0.0, 0.0)
	settle(w, far)
	await get_tree().physics_frame
	var after: Dictionary = w.debug_physics_stats()
	# The set is bounded by the radius, not by how far the player has walked.
	assert_int(after["bodies"]).is_less_equal(before["bodies"] + 4)
	# ...and the ground the player left is no longer collidable.
	assert_bool(ray(Vector3(CENTER.x, 80.0, CENTER.z),
		Vector3(CENTER.x, 20.0, CENTER.z)).is_empty()).is_true()
	# ...while the ground they arrived on is.
	assert_bool(ray(Vector3(far.x, 80.0, far.z), Vector3(far.x, 20.0, far.z)).is_empty()).is_false()
	w.free()

func test_an_empty_chunk_costs_no_body_and_is_not_retried(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var st: Dictionary = w.debug_physics_stats()
	# The probe is conservative, so some resident candidates mesh to nothing; those release
	# their slot instead of holding a body, and the cached verdict stops them coming back.
	assert_int(st["bodies"]).is_less_equal(st["chunks_resident"])
	for i in range(20):
		assert_int(w.debug_physics_frame(CENTER)).is_equal(0)
	w.free()
