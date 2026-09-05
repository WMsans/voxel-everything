extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Spec section 6: "Edit -> remesh -> collidable again in 1-2 frames." The collider is
# rebuilt from the same op list the renderer uses, so what you shot through is what you fall
# through.

const CENTER := Vector3(60.0, 55.0, 60.0)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.physics_radius_m = 25.0
	# A 25 m ball holds ~163 of today's 6.4 m chunks, where it held a fifth of that when a
	# chunk was 12.8 m. 64 slots can no longer cover it, and a pool that is permanently full
	# tests eviction churn rather than what these tests are about.
	w.max_collider_chunks = 512
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# Settled means the streamer owes nothing: no resident chunk still waiting for a collider and
# no meshed result waiting to become a shape.
#
# It used to mean "several consecutive frames that took no action", which stopped being true
# when the mesher moved onto its own thread: run_frame returns 0 on every frame it spends
# WAITING for the worker, so the old helper declared victory after five frames with 163 chunks
# pending and not a single body built, and every assertion downstream of it was measuring an
# empty world. Frames are cheap (this drains in ~1300 of them, well under a second), so the
# budget is a ceiling on a stuck run rather than a target.
func settle(w: VoxelWorld, center: Vector3, frames := 6000) -> bool:
	var quiet := 0
	for i in range(frames):
		w.hooks().debug_physics_frame(center)
		var st := w.hooks().debug_physics_stats()
		quiet = quiet + 1 if st["chunks_pending"] == 0 and st["queued"] == 0 else 0
		if quiet >= 4:
			return true
	return false

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
	# A blast dirties a handful of chunks and the streamer rebuilds them nearest-first, so the
	# one under the player comes back first. Since the mesher moved onto its own thread a
	# streamer "frame" is microseconds and no longer stands in for the spec's "1-2 frames" of
	# wall clock, so this waits for the rebuild to actually land rather than counting frames.
	assert_bool(settle(w, CENTER)).override_failure_message(
		"the streamer never finished rebuilding the crater: %s" % w.hooks().debug_physics_stats()
		).is_true()
	await get_tree().physics_frame

	var after := ray(from, to)
	assert_bool(after.is_empty()).override_failure_message(
		"the crater floor has no collider at all").is_false()
	assert_float(after["position"].y).override_failure_message(
		"collision did not follow the crater: %f vs %f" % [after["position"].y, surface]
		).is_less(surface - 1.0)
	# ...and it still agrees with the field the renderer draws.
	var oracle: Dictionary = w.hooks().debug_raycast(from, Vector3(0, -1, 0))
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
	var hit: Dictionary = w.hooks().debug_raycast(Vector3(CENTER.x, 80.0, CENTER.z), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()

	# Paint changes no SDF, but it is still an op in the region's list, so the chunks it
	# touches are re-meshed once. The point of the test is that it CONVERGES — a dirty mark
	# that never clears would keep the mesher busy for ever.
	tool.apply_sphere_paint(hit["pos"], 3.0, 1)
	settle(w, CENTER)
	for i in range(20):
		assert_int(w.hooks().debug_physics_frame(CENTER)).is_equal(0)
