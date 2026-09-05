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
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# The flag word has to agree with the mip chain it summarises for every generated brick, or
# the marcher skips ground that is there. debug_brick_flags returns, per resident brick of
# one region: the flag word the GPU wrote, and the word ve::brick_flags_from_mips computes
# from the CPU reference brick. They must match everywhere.
func test_gpu_flags_match_the_cpu_reference(timeout := 30000) -> void:
	var w := make_world()
	w.hooks().debug_stream_region(Vector3i(1, 2, 1))
	var d: Dictionary = w.hooks().debug_brick_flags(Vector3i(1, 2, 1))
	assert_int(int(d["compared"])).is_greater(50)
	assert_int(int(d["mismatches"])).is_equal(0)

# A brick allocated but not yet generated must read as "march it": the conservative value is
# what stops a dropped generation job from becoming a hole.
func test_allocated_but_ungenerated_bricks_are_conservative() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_brick_flags_after_mark(Vector3i(1, 2, 1))
	assert_int(int(d["allocated"])).is_greater(0)
	assert_int(int(d["non_conservative"])).is_equal(0)
