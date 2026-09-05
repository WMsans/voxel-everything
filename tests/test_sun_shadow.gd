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
	# The walk needs whole in-radius sibling sets (L6 spans 819 m); this suite's
	# cameras need ~1310 m (measured to the farthest L6 sibling), and the bracket needs
	# the map to hold ground to 360 m -- so 1400 with margin.
	w.stream_radius_m = 1400.0
	w.max_lod_pages = 16384
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# The bias bug scales with the shadow map's texel size. At the 1400 m stream radius the
# ortho spans 2800 m, so one texel is 1.37 m of world and the broken bias is
#   texel_world * depth_range * 0.5 = 1.37 * 4564 * 0.5 = ~3100 m
# which is far deeper than the probe, so metre-scaled bias reports LIT. The fixed bias
# stays in the centimetres. make_big_world funds the pool (16384 pages) so the map holds
# dense ground all the way out to the bracket's 360 m samples.
func make_big_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	# Same horizon; the funded pool holds the dense ground the bracket samples to 360 m.
	w.stream_radius_m = 1400.0
	w.max_lod_pages = 16384
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

# The map is rasterized from the LoD mesh and from nothing else, so it may only shade the
# pixels that mesh drew. The near field's surface is the fine field, which sits metres away
# from the mesh: tested against this map, open sunlit ground inside the near field reported
# shadow. Sample sunlit surface points near the camera and require the shading path to leave
# them lit; the raw map (debug_sun_shadow_visibility) is free to say whatever it rasterized.
func test_the_lod_map_does_not_shadow_the_near_field(timeout := 120000) -> void:
	var w := make_big_world()
	var cam := Vector3(60, 90, 60)
	assert_bool(await settle(w, cam, Vector3(1, -0.3, 1).normalized())).is_true()
	w.hooks().debug_sun_shadow_build(true)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_greater(0)
	var sun := Vector3(0.5746958, 0.7662610, 0.2873479) # ve::kSunDir
	var fade_start: float = w.hooks().debug_lod_fade_band().x

	var checked := 0
	var darkened := 0
	for x in range(24, 121, 8):
		for z in range(24, 121, 8):
			var hit: Dictionary = w.hooks().debug_raycast(Vector3(x, 200.0, z), Vector3(0, -1, 0))
			if not hit["hit"]:
				continue
			var p: Vector3 = hit["pos"]
			var n: Vector3 = hit["normal"]
			# Only ground the sun can reach, and only inside the near field the raymarcher owns.
			if n.dot(sun) <= 0.25 or p.distance_to(cam) >= fade_start:
				continue
			# The fine field is the near field's own geometry: if nothing solid is above this
			# point along the sun ray, the pixel is lit and no shadow term may darken it.
			if w.hooks().debug_raycast(p + n * 0.15, sun)["hit"]:
				continue
			checked += 1
			if w.hooks().debug_sun_shadow_shading(p + n * 0.05, cam) < 0.5:
				darkened += 1
	assert_int(checked).override_failure_message(
		"no sunlit near-field ground was sampled; the fixture, not the shader, is wrong").is_greater(50)
	assert_int(darkened).override_failure_message(
		"the LoD sun map darkened %d of %d sunlit near-field points" % [darkened, checked]).is_equal(0)

# The map has to hold the GROUND -- not a wireframe of it, and not a coarser surface floating
# over it. Both failures shipped, and neither was visible to any test here:
#
#  * SunShadowPass borrowed LodRasterPass::front_face_clockwise(), a winding measured against
#    the CAMERA's reverse-Z perspective. The sun's ortho has the opposite handedness, so the
#    pass culled exactly the up-facing terrain quads it exists to record; all that reached the
#    map were the skirt curtains, which lod_append_skirts emits twice with opposite winding.
#    test_something_actually_gets_drawn_into_it passed on those skirts alone.
#  * The map was rasterized from every RESIDENT page, so the never-evicted level 5-7 ancestors
#    (12.8-51.2 m cells) wrote their tent-filtered surfaces over ground their own children
#    already described, 2 to 15 m too high, and shadowed open sunlit terrain.
#
# One bracket catches both: at each sampled patch of ground the stored surface must sit within
# BRACKET_M of the real one. Too low (or absent) and the buried probe reads lit; too high and
# the airborne probe reads shadowed.
#
# The ground is computed rather than raycast on purpose. debug_raycast reads the brick field,
# which only exists inside the near field's residency radius -- the very region the far field
# never builds -- so it cannot see the ground this map is made of. ve::AnalyticGenerator is a
# pure height field, mirrored here as test_material_seam.gd already mirrors it.
const BRACKET_M := 8.0

func terrain_height(x: float, z: float) -> float:
	return 51.2 + 6.0 * sin(x * 0.11) * cos(z * 0.13) \
		+ 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043) \
		+ sin(x * 0.23 + z * 0.19)

func terrain_normal(x: float, z: float) -> Vector3:
	const E := 0.05
	var dhdx := (terrain_height(x + E, z) - terrain_height(x - E, z)) / (2.0 * E)
	var dhdz := (terrain_height(x, z + E) - terrain_height(x, z - E)) / (2.0 * E)
	return Vector3(-dhdx, 1.0, -dhdz).normalized()

# Does anything stand between this point and the sun? March the height field; a genuinely
# shadowed patch says nothing about where the stored surface is, so it must be skipped.
func sun_is_clear(p: Vector3, sun: Vector3) -> bool:
	for i in range(1, 400):
		var q := p + sun * (i * 0.5)
		if q.y > 80.0:
			return true
		if q.y < terrain_height(q.x, q.z):
			return false
	return true

func test_the_map_holds_the_ground_within_a_few_metres(timeout := 120000) -> void:
	var w := make_big_world()
	var cam := Vector3(60, 90, 60)
	assert_bool(await settle(w, cam, Vector3(1, -0.3, 1).normalized())).is_true()
	w.hooks().debug_sun_shadow_build(true)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_greater(0)
	const SUN := Vector3(0.5746958, 0.7662610, 0.2873479) # ve::kSunDir

	var checked := 0
	var too_low := 0   # nothing above a buried point: the map is missing that ground
	var too_high := 0  # an occluder over open ground: the map sits above the terrain
	for x in range(40, 361, 20):
		for z in range(40, 361, 20):
			var n := terrain_normal(x, z)
			if n.dot(SUN) <= 0.25:
				continue
			var p := Vector3(x, terrain_height(x, z), z)
			if not sun_is_clear(p + n * 0.15, SUN):
				continue
			checked += 1
			if w.hooks().debug_sun_shadow_visibility(p - SUN * BRACKET_M) > 0.5:
				too_low += 1
			if w.hooks().debug_sun_shadow_visibility(p + SUN * BRACKET_M) < 0.5:
				too_high += 1
	assert_int(checked).override_failure_message(
		"no sunlit ground was sampled; the fixture, not the map, is wrong").is_greater(50)
	assert_int(too_low).override_failure_message(
		"%d of %d probes %.0f m UNDER the surface read lit: the map does not hold that ground"
		% [too_low, checked, BRACKET_M]).is_less_equal(checked / 20)
	assert_int(too_high).override_failure_message(
		"%d of %d probes %.0f m ABOVE open ground read shadowed: the map sits over the terrain"
		% [too_high, checked, BRACKET_M]).is_less_equal(checked / 20)
