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
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# The bias bug scales with world size, which is why make_world()'s {8,5,8} world passes
# against it: there the broken bias is ~20 m of world depth and the probe sits ~34 m under
# the surface, so it still reads shadowed. At {16,5,16} the broken bias is
#   texel_world * depth_range * 0.5 = 0.268 * 451.2 * 0.5 = ~60 m
# which is deeper than the probe, so the old code reports LIT. The fixed bias is ~0.13 m.
func make_big_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(16, 5, 16)
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8
const POOL_STABLE_TICKS := 120

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	var pool_quiet := 0
	var last_draw_pages := -1
	var last_requests_pending := -1
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d: Dictionary = w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
		# A bounded pool can leave a stable, drawable coarse view with requests that
		# cannot be funded. Treat that as settled only after the visible state has
		# stopped changing for a longer streak; this avoids accepting a transient stall.
		var pool_starved: bool = d["pages_free"] < 32 and d["requests_pending"] > 0 \
			and d["dirty_chunks"] == 0 and d["partial_allocations"] == 0
		var same_view: bool = d["draw_pages"] == last_draw_pages \
			and d["requests_pending"] == last_requests_pending
		pool_quiet = pool_quiet + 1 if pool_starved and same_view else 0
		if pool_quiet >= POOL_STABLE_TICKS:
			return true
		last_draw_pages = d["draw_pages"]
		last_requests_pending = d["requests_pending"]
	return false

func test_the_map_is_the_stated_size_and_covers_the_world() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_sun_shadow_stats()
	assert_int(d["size"]).is_equal(2048)
	assert_bool(d["ortho_valid"]).is_true()
	assert_float(d["texel_world"]).is_between(0.1, 20.0)

# The whole point of a bounded world and a fixed sun: the matrix never changes, so nothing
# can shimmer and no rebuild is ever needed for camera motion alone.
func test_the_matrix_does_not_move_with_the_camera(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.hooks().debug_sun_shadow_build(true)
	var a: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]
	assert_bool(await settle(w, Vector3(180, 90, 40), Vector3(-1, -0.4, 0).normalized())).is_true()
	var b: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]
	for i in range(16):
		assert_float(b[i]).is_equal_approx(a[i], 1e-6)

func test_something_actually_gets_drawn_into_it(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.hooks().debug_sun_shadow_build(true)
	var d: Dictionary = w.hooks().debug_sun_shadow_stats()
	assert_int(d["rebuilds"]).is_greater(0)
	assert_bool(d["map_valid"]).is_true()
	# A point well under the terrain surface is behind whatever the map recorded above it.
	# A point well above everything is not.
	assert_float(w.hooks().debug_sun_shadow_visibility(Vector3(60.0, 20.0, 60.0))).is_equal_approx(0.0, 0.01)
	assert_float(w.hooks().debug_sun_shadow_visibility(Vector3(60.0, 300.0, 60.0))).is_equal_approx(1.0, 0.01)

func test_the_bias_is_in_depth_units_not_metres(timeout := 120000) -> void:
	var w := make_big_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.hooks().debug_sun_shadow_build(true)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_greater(0)
	# Well under the terrain surface: shadowed. Fails against a metres-scaled bias.
	assert_float(w.hooks().debug_sun_shadow_visibility(Vector3(60.0, 20.0, 60.0))) \
		.is_equal_approx(0.0, 0.01)

func test_a_lazy_rebuild_does_not_fire_every_frame(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	# The pass starts dirty. Eleven calls are still inside the minimum-frame gate.
	for i in range(11):
		w.hooks().debug_sun_shadow_build(false)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_equal(0)
	w.hooks().debug_sun_shadow_build(false)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_equal(1)
	# Once clean, ordinary calls remain refused until LoD dirties it again.
	for i in range(5):
		w.hooks().debug_sun_shadow_build(false)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_equal(1)

func test_turning_the_sun_map_off_lights_everything(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.hooks().debug_sun_shadow_build(true)
	w.set_effect_enabled("sun_shadow_map", false)
	assert_float(w.hooks().debug_sun_shadow_visibility(Vector3(60.0, 20.0, 60.0))).is_equal_approx(1.0, 0.01)
