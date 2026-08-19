extends GdUnitTestSuite

# Spec section 5 step 3 and section 6's dynamic-body rules: a component becomes a Jolt rigid
# body carrying a box compound, with mass from its solid volume, an explosion's impulse
# already applied, and a sleep clock the re-merge hook (Task 13) reads.

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
	w.max_collider_chunks = 64
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func settle_colliders(w: VoxelWorld, center: Vector3, frames := 400) -> void:
	# Run the full budget rather than stopping on a quiet streak: a quiet streak can occur
	# while a mesh batch is in flight, before the first static body exists.
	for i in range(frames):
		w.debug_physics_frame(center)

func test_a_spawned_body_has_mass_and_falls(timeout := 90000) -> void:
	var w := make_world()
	var centre := Vector3(20.0, 56.0, 20.0)
	settle_colliders(w, centre)
	# A lump of rock lifted 20 m into the air, with no kick: it should just drop.
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, false)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	assert_float(d["mass"]).is_greater(0.0)
	assert_int(d["shapes"]).is_greater(0)
	assert_int(d["shapes"]).is_less_equal(64)
	var start: Vector3 = d["origin"]

	for i in range(30):
		await get_tree().physics_frame
	var s: Dictionary = w.debug_test_body_stats(d["index"])
	assert_bool(s["live"]).is_true()
	assert_float((s["origin"] as Vector3).y).override_failure_message(
		"the body did not fall").is_less(start.y - 0.5)

func test_a_body_lands_on_the_streamed_collider_and_sleeps(timeout := 120000) -> void:
	var w := make_world()
	var centre := Vector3(20.0, 56.0, 20.0)
	settle_colliders(w, centre)
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 8.0, 0.0), Vector3.ZERO, false)
	assert_bool(d.get("ok", false)).is_true()
	for i in range(400):
		await get_tree().physics_frame
		w.debug_tick_test_bodies(1.0 / 60.0)
		if w.debug_test_body_stats(d["index"])["asleep_s"] > 0.5:
			break
	var s: Dictionary = w.debug_test_body_stats(d["index"])
	assert_float(s["asleep_s"]).override_failure_message(
		"the body never came to rest on the terrain").is_greater(0.5)
	# It stopped ON the ground, not below it.
	var oracle: Dictionary = w.debug_raycast(Vector3(centre.x, 90.0, centre.z), Vector3(0, -1, 0))
	assert_float((s["origin"] as Vector3).y).is_greater((oracle["pos"] as Vector3).y - 2.0)

func test_an_impulse_throws_the_body_sideways(timeout := 90000) -> void:
	var w := make_world()
	settle_colliders(w, Vector3(20.0, 56.0, 20.0))
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3(400.0, 0.0, 0.0), false)
	assert_bool(d.get("ok", false)).is_true()
	var start: Vector3 = d["origin"]
	for i in range(20):
		await get_tree().physics_frame
	var s: Dictionary = w.debug_test_body_stats(d["index"])
	assert_float((s["origin"] as Vector3).x).override_failure_message(
		"the explosion impulse did not reach the body").is_greater(start.x + 0.5)

func test_islands_and_debris_get_cel_render_instances(timeout := 90000) -> void:
	var w := make_world()
	settle_colliders(w, Vector3(20.0, 56.0, 20.0))
	var rock: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, false)
	var crumb: Dictionary = w.debug_spawn_test_body(Vector3i(28, 61, 28), Vector3i(28, 61, 28),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, true)
	assert_bool(rock.get("ok", false)).is_true()
	assert_bool(crumb.get("ok", false)).is_true()
	assert_bool(rock["has_render_mesh"]).override_failure_message(
		"island did not build its cel render mesh").is_true()
	assert_int(rock["render_tris"]).is_greater(0)
	assert_bool(rock["cel_material"]).override_failure_message(
		"island fell back to StandardMaterial3D").is_true()
	assert_bool(crumb["has_render_mesh"]).is_true() # dual-contoured, drawn by RenderingServer
	assert_int(crumb["render_tris"]).is_greater(0)
	assert_bool(crumb["cel_material"]).override_failure_message(
		"debris fell back to StandardMaterial3D").is_true()

func test_despawn_removes_the_body(timeout := 90000) -> void:
	var w := make_world()
	settle_colliders(w, Vector3(20.0, 56.0, 20.0))
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, false)
	assert_bool(d.get("ok", false)).is_true()
	w.debug_despawn_test_body(d["index"])
	assert_bool(w.debug_test_body_stats(d["index"])["live"]).is_false()
