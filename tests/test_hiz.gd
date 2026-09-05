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
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

func test_the_pyramid_is_fixed_size_and_fully_mipped() -> void:
	var w := make_world()
	var d := w.hooks().debug_hiz_stats()
	assert_int(d["width"]).is_equal(256)
	assert_int(d["height"]).is_equal(256)
	assert_int(d["mips"]).is_equal(9)
	assert_int(d["readback_level"]).is_equal(3)
	assert_int(d["readback_texels"]).is_equal(32 * 32)

# Reverse-Z: nearer is LARGER, so the conservative reduction is a MIN -- it keeps the
# FARTHEST of the nearest surfaces, which is the only value a whole footprint can be tested
# against without wrongly hiding something.
func test_the_reduction_is_a_min_in_reverse_z() -> void:
	var w := make_world()
	# A synthetic depth image: one near texel (0.9) in a far field (0.1).
	var d := w.hooks().debug_hiz_probe_synthetic(0.1, 0.9)
	assert_float(d["mip0_at_near_texel"]).is_equal_approx(0.9, 0.001)
	# The parent covering both must keep the FAR value.
	assert_float(d["mip1_covering_both"]).is_equal_approx(0.1, 0.001)
	assert_float(d["top_mip"]).is_equal_approx(0.1, 0.001)

# The whole point of the split in spec section 6.3: stale occlusion may delay a build, never
# hide a chunk. A box in front of every occluder must never test occluded.
func test_a_box_in_front_of_everything_is_never_occluded() -> void:
	var w := make_world()
	w.hooks().debug_hiz_probe_synthetic(0.1, 0.1) # everything far
	# ss box covering the whole screen, nearest depth 0.9 (well in front).
	assert_bool(w.hooks().debug_hiz_occluded(Vector2(0.0, 0.0), Vector2(1.0, 1.0), 0.9)).is_false()
	# ...and the same box behind everything is.
	w.hooks().debug_hiz_probe_synthetic(0.9, 0.9) # everything near
	assert_bool(w.hooks().debug_hiz_occluded(Vector2(0.0, 0.0), Vector2(1.0, 1.0), 0.1)).is_true()

func test_an_absent_readback_never_occludes() -> void:
	var w := make_world()
	# Before any build has landed there is no data; the safe answer is "visible".
	assert_bool(w.hooks().debug_hiz_occluded(Vector2(0.2, 0.2), Vector2(0.3, 0.3), 0.01)).is_false()
