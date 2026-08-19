extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func test_shutdown_drains_pending_hiz_readback_before_freeing_resources() -> void:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = true
	world.physics_enabled = false
	add_child(world)
	_worlds.append(world)

	var result: Dictionary = world.debug_hiz_shutdown_probe()
	assert_bool(result["callback_guarded"]).is_true()
	assert_bool(result["queued"]).is_true()
	assert_bool(result["was_pending"]).is_true()
	assert_bool(result["drained"]).is_true()
	assert_bool(result["initialized_after"]).is_false()
	world.ensure_initialized()
	assert_bool(world.is_initialized()).is_false()
