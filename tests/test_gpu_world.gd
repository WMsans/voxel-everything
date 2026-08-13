extends GdUnitTestSuite

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.world_size_bricks = Vector3i(20, 12, 20)
	add_child(w)
	return w

func test_world_initializes_and_uploads() -> void:
	var w := make_world()
	w.ensure_initialized()
	assert_bool(w.is_initialized()).is_true()
	assert_bool(w.debug_indirection_tex().is_valid()).is_true()
	assert_bool(w.debug_sdf_atlas().is_valid()).is_true()

func test_gpu_readback_returns_data() -> void:
	var w := make_world()
	w.ensure_initialized()
	var rd := w.debug_local_rd() as RenderingDevice
	assert_object(rd).is_not_null()
	# Verified against Godot 4.7.1 engine source: a 3D texture has exactly one layer
	# (texture.layers = 1), so texture_get_data(tex, 0) returns the FULL volume
	# (width*height*depth*bytes), not a single 2D slice.
	var slice0: PackedByteArray = rd.texture_get_data(w.debug_sdf_atlas(), 0)
	assert_int(slice0.size()).is_equal(512 * 256 * 512) # R8: 1 byte per texel, full 3D volume
	var ind0: PackedByteArray = rd.texture_get_data(w.debug_indirection_tex(), 0)
	assert_int(ind0.size()).is_equal(20 * 12 * 20 * 4) # R32_SINT: 4 bytes per texel, full volume

func test_reinit_after_remove_and_readd() -> void:
	# Regression: _exit_tree() used to leave initialized_/gpu_/world_ stale, so a
	# remove_child/add_child cycle made ensure_initialized() early-return with a
	# torn-down GpuWorld (freed RIDs) -> silently dead compositor path.
	var w := make_world()
	w.ensure_initialized()
	assert_bool(w.is_initialized()).is_true()
	var parent := w.get_parent()
	parent.remove_child(w)
	parent.add_child(w)
	w.ensure_initialized()
	assert_bool(w.is_initialized()).is_true()
	assert_bool(w.debug_indirection_tex().is_valid()).is_true()
