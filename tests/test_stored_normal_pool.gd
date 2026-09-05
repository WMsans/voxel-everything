extends GdUnitTestSuite

# Task 6's fixed-capacity StoredNormalPool, exercised through a small test world whose
# normal-pool budget is shrunk to 64 KiB by the debug initializer. Three deterministic
# 17^3 override-normal payloads (kBrickSdfCount uint16 samples = 9826 bytes, rounded to
# 9828) must allocate distinct non-negative offsets, a release must be reused first-fit,
# and one payload more than the remainder must fail exactly once and enter the wide R8
# fallback exactly once. Fail-soft: an unallocatable payload can never reject geometry.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_small_world(normal_budget: int) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	# Debug initializer: must land BEFORE the atlas is created.
	w.hooks().debug_set_normal_pool_budget(normal_budget)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).override_failure_message(
		"atlas with %d-byte normal pool did not initialize" % normal_budget).is_true()
	return w

func make_default_world() -> VoxelWorld:
	# TRUE default settings: full 64x32x32 atlas, 512 region slots, 32 MiB normal pool.
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

func _packed_normals(seed: int) -> PackedByteArray:
	var samples := 17 * 17 * 17 # kBrickSdfCount
	var bytes := PackedByteArray()
	bytes.resize(samples * 2)
	for i in range(samples):
		@warning_ignore("integer_division")
		var v: int = (seed * 2654435761 + i * 97) % 65536
		bytes.encode_u16(i * 2, v)
	return bytes

func test_small_pool_allocates_reuses_and_falls_soft() -> void:
	var w := make_small_world(65536)
	var o0: int = w.hooks().debug_normal_upload_override(0, _packed_normals(1))
	var o1: int = w.hooks().debug_normal_upload_override(1, _packed_normals(2))
	var o2: int = w.hooks().debug_normal_upload_override(2, _packed_normals(3))
	assert_int(o0).is_greater_equal(0)
	assert_int(o1).is_greater_equal(0)
	assert_int(o2).is_greater_equal(0)
	assert_bool(o0 != o1 and o1 != o2 and o0 != o2).override_failure_message(
		"offsets not distinct: %d %d %d" % [o0, o1, o2]).is_true()

	# Release one span; the next equal-sized upload must reuse it (first fit).
	w.hooks().debug_normal_release_override(1)
	var o3: int = w.hooks().debug_normal_upload_override(3, _packed_normals(4))
	assert_int(o3).is_greater_equal(0)

	# Exhaust what remains: every 17^3 payload needs 9828 aligned bytes and only a
	# fragment is left after three live spans plus metadata.
	var o4: int = w.hooks().debug_normal_upload_override(4, _packed_normals(5))
	assert_int(o4).is_equal(-1)

	var stats: Dictionary = w.hooks().debug_stored_normal_stats()
	assert_int(stats["capacity_bytes"]).is_equal(65536)
	assert_int(stats["live_bytes"]).is_less_equal(65536)
	assert_int(stats["allocation_failures"]).is_equal(1)
	assert_int(stats["fallback_hits"]).is_equal(1)

func test_default_settings_memory_bounds() -> void:
	var w := make_default_world()
	var stats: Dictionary = w.hooks().debug_stored_normal_stats()
	# The global constraint: compact-normal render-device allocation fixed at exactly
	# 32 MiB including both offset tables; never grows implicitly.
	assert_int(stats["capacity_bytes"]).is_equal(33554432)
	assert_int(stats["live_bytes"]).is_less_equal(33554432)
	assert_int(stats["allocation_failures"]).is_equal(0)
	assert_int(stats["fallback_hits"]).is_equal(0)
	# The R8 SDF atlas stays byte-for-byte unchanged: 1088 x 544 x 544 x 1 byte.
	var atlas: Dictionary = w.hooks().debug_atlas_stats()
	assert_int(atlas["sdf_atlas_bytes"]).is_equal(1088 * 544 * 544)
