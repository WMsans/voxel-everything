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

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	# The primary camera is the caller's view. If a bounded pool cannot fund its far
	# refinement, retry from the exterior-facing view; this still drives the real tick
	# path and must meet the same strict readiness condition.
	var cameras := [fwd, Vector3(-1, -0.3, -1).normalized()]
	for camera in cameras:
		var quiet := 0
		for i in range(SETTLE_BUDGET):
			w.hooks().debug_lod_tick(pos, camera)
			await get_tree().process_frame
			var d: Dictionary = w.hooks().debug_lod_stats()
			quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
			if quiet >= QUIET_TICKS:
				return true
	push_error("strict settle timeout: %s" % w.hooks().debug_lod_stats())
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

# A day/night sweep must not lag twelve frames behind the sun. kMinFrames exists to stop LoD
# churn from rebuilding constantly; a sun that actually moved is not churn.
func test_a_moved_sun_rebuilds_without_waiting_for_the_throttle(timeout := 60000) -> void:
	var w := make_world()
	var light := DirectionalLight3D.new()
	light.rotation = Vector3(-0.9, 0.3, 0.0)
	add_child(light)
	w.sun_light_path = w.get_path_to(light)
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	# Settle the map so the pass is clean and inside its throttle window.
	w.hooks().debug_sun_shadow_build(true)
	var before: int = w.hooks().debug_sun_shadow_stats()["rebuilds"]
	var matrix_before: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]
	# A clean pass refuses an unforced build...
	w.hooks().debug_sun_shadow_build(false)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_equal(before)
	# ...but not when the sun has moved.
	light.rotation = Vector3(-0.5, 1.4, 0.0)
	await get_tree().process_frame
	w.hooks().debug_sun_shadow_build(false)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_equal(before + 1)
	var matrix_after: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]
	var moved := false
	for i in range(16):
		if absf(matrix_after[i] - matrix_before[i]) > 1e-5:
			moved = true
	assert_bool(moved).is_true()
	light.queue_free()

# The whole point: rotating the light must change what is in shadow. Rather than betting on
# one hand-picked point flipping over procedural terrain, sample a grid and assert the SET of
# shadowed points differs. Over a 40-degree sun swing across real terrain, some must flip.
func _shadow_mask(w: VoxelWorld) -> Array:
	var mask: Array = []
	for x in range(40, 121, 20):
		for z in range(40, 121, 20):
			# Just under the surface band, where occlusion actually varies with sun angle.
			mask.append(w.hooks().debug_sun_shadow_visibility(Vector3(float(x), 50.0, float(z))))
	return mask

func test_moving_the_sun_moves_the_shadow(timeout := 60000) -> void:
	var w := make_world()
	var light := DirectionalLight3D.new()
	add_child(light)
	w.sun_light_path = w.get_path_to(light)
	light.rotation = Vector3(-0.35, 0.0, 0.0) # low sun
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	await get_tree().process_frame
	w.hooks().debug_sun_shadow_build(true)
	var low: Array = _shadow_mask(w)
	var low_matrix: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]

	light.rotation = Vector3(-1.2, 2.4, 0.0) # high sun, opposite azimuth
	await get_tree().process_frame
	w.hooks().debug_sun_shadow_build(true)
	var high: Array = _shadow_mask(w)
	var high_matrix: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]

	# The projection followed the node.
	var matrix_moved := false
	for i in range(16):
		if absf(high_matrix[i] - low_matrix[i]) > 1e-5:
			matrix_moved = true
	assert_bool(matrix_moved).override_failure_message(
		"the sun ortho did not change when the light rotated").is_true()

	# And so did the image.
	var flipped := 0
	for i in range(low.size()):
		if absf(float(high[i]) - float(low[i])) > 0.5:
			flipped += 1
	assert_int(flipped).override_failure_message(
		"no sampled point changed shadow state across a 40-degree sun swing").is_greater(0)
	light.queue_free()
