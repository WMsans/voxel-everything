extends GdUnitTestSuite

# With one octant admitted per tick, mesh compilation must be paid inside that
# octant's build timer. A later atomic commit must not compile the entire chunk.
func test_shape_compilation_is_paid_during_staging(timeout := 60000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.physics_radius_m = 12.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 1
	add_child(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var worst_unaccounted := 0.0
	var saw_body := false
	var quiet := 0
	for i in range(10000):
		w.hooks().debug_physics_frame(Vector3(60.0, 55.0, 60.0))
		var perf: Dictionary = w.hooks().debug_perf_stats()
		worst_unaccounted = maxf(worst_unaccounted,
			float(perf["phys_apply_ms"]) - float(perf["build_ms"]))
		var stats: Dictionary = w.hooks().debug_physics_stats()
		saw_body = saw_body or int(stats["bodies"]) > 0
		quiet = quiet + 1 if saw_body and int(stats["chunks_pending"]) == 0 and int(stats["queued"]) == 0 else 0
		if quiet >= 4:
			break
		# Give the asynchronous mesher time to finish on fast polling hosts.
		if i % 16 == 0:
			OS.delay_msec(1)
	w.free()
	assert_bool(saw_body).is_true()
	assert_int(quiet).override_failure_message("the measured collider workload did not finish").is_greater_equal(4)
	assert_float(worst_unaccounted).override_failure_message(
		"%.2f ms of collider work escaped the octant build timer" % worst_unaccounted
	).is_less(4.0)
