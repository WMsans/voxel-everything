extends GdUnitTestSuite

# Task 2: the per-brick baked-normal cache. The brick generator bakes the field gradient
# into a 20 cm lattice mirroring the SDF atlas slot for slot; the raymarcher reads it with one
# filtered fetch at the hit instead of re-running the terrain pipeline for the normal.

const FJORDS := "res://assets/pipelines/fjords.pipeline"

var _worlds: Array[VoxelWorld] = []

func after_test() -> void:
	for world in _worlds:
		if is_instance_valid(world):
			world.free()
	_worlds.clear()

# Same world as test_normal_artifact.gd's make_world(): a small local-device atlas streamed
# over the hills terrain, which every surface brick should cache.
func make_world() -> VoxelWorld:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = true
	world.physics_enabled = false
	world.atlas_bricks = Vector3i(32, 16, 32)
	world.max_region_slots = 64
	add_child(world)
	_worlds.append(world)
	assert_bool(world.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for frame in range(500):
		quiet = quiet + 1 if world.hooks().debug_stream_frame(Vector3(20, 56, 20)) == 0 else 0
		if quiet >= 6:
			break
	return world

func _material(world: VoxelWorld, name: String) -> int:
	var names: Array = world.material_table().map(func(m): return m["name"])
	return names.find(name) + 1

# Forest hits on a 61 x 61 grid at 25 m over +-750 m, as test_conifers.gd does.
func _forest_hits(world: VoxelWorld) -> Array:
	var forest := _material(world, "forest")
	var out := []
	for i in range(-30, 31):
		for j in range(-30, 31):
			var hit: Dictionary = world.raycast(Vector3(i * 25.0, 800.0, j * 25.0), Vector3.DOWN, 1600.0)
			if hit["hit"] and int(hit["material"]) == forest:
				out.append(hit["pos"])
	return out

func test_streamed_surface_bricks_are_normal_cached() -> void:
	var world := make_world()
	var s: Dictionary = world.hooks().debug_brick_cache_stats()
	assert_int(s["normal_ok"]).is_greater(100)
	assert_int(s["sun_ok"]).is_equal(0) # nothing bakes sun until Task 3

func test_a_reallocated_slot_starts_with_no_cache() -> void:
	# A streamed surface brick carries a baked normal. Give its region slot back, then mark the
	# brick again WITHOUT generating it: the slot that is reallocated still holds the last
	# owner's texture bytes, so brick_mark must have cleared the cache words. (The free list is
	# LIFO, so the slot that comes back is normally the one just released.)
	var world := make_world()
	var hit: Dictionary = world.raycast(Vector3(20.0, 80.0, 20.0), Vector3.DOWN, 200.0)
	assert_bool(hit["hit"]).override_failure_message("no ground under the camera").is_true()
	var brick := Vector3i((hit["pos"] / 0.8).floor())
	var region := Vector3i(brick.x >> 5, brick.y >> 5, brick.z >> 5)
	var rslot := world.hooks().debug_region_map_entry(region)
	assert_int(rslot).override_failure_message("the brick's region is not streamed").is_greater_equal(0)
	assert_bool((world.hooks().debug_brick_cache_flags(brick) & 1) != 0).override_failure_message(
		"the streamed brick has no normal cache: %s" % str(brick)).is_true()
	world.hooks().debug_release_region(rslot)
	world.hooks().debug_mark_region(region, rslot, brick, brick, 0, true)
	assert_int(world.hooks().debug_brick_cache_flags(brick) & 1).override_failure_message(
		"a reallocated slot kept a stale normal cache").is_equal(0)

func test_cached_trunk_normals_are_no_worse_than_today() -> void:
	# Fjords seed 0: find a trunk as test_trunks_are_bark does, stream there, then probe
	# 4 m in front of it looking at it, 15 deg down, 160x120.
	# Run debug_raymarch_normal_probe twice: brick_cache off, then on.
	# 20 cm is the accepted trunk resolution (spec accepted trade 1), so this compares against
	# an absolute bound instead of the exact analytic path: `off` is the analytic normal, which
	# no 20 cm lattice can match on a tapering 0.45 m trunk.
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.terrain_pipeline_path = FJORDS
	world.world_seed = 0
	world.use_local_device = true
	world.physics_enabled = false
	world.stream_radius_m = 2600.0
	add_child(world)
	_worlds.append(world)
	assert_bool(world.hooks().debug_init_atlas()).is_true()
	assert_int(world.hooks().debug_pump_sectors(Vector3.ZERO, 1600.0, 900)).is_greater(0)

	var bark := _material(world, "bark")
	var trunk := Vector3.INF
	var trunk_dir := Vector3.RIGHT
	for p in _forest_hits(world):
		for dir in [Vector3.RIGHT, Vector3.LEFT, Vector3.FORWARD, Vector3.BACK]:
			var hit: Dictionary = world.raycast(p + Vector3(0.0, 6.0, 0.0) - dir * 6.0, dir, 12.0)
			if hit["hit"] and int(hit["material"]) == bark:
				trunk = hit["pos"]
				trunk_dir = dir
				break
		if trunk != Vector3.INF:
			break
	assert_bool(trunk != Vector3.INF).override_failure_message("no trunk found").is_true()

	world.hooks().debug_pump_sectors(trunk, 900.0, 600)
	var quiet := 0
	for i in range(600):
		quiet = quiet + 1 if world.hooks().debug_stream_frame(trunk + Vector3(0.0, 20.0, 0.0)) == 0 else 0
		if quiet >= 6:
			break

	var eye := trunk + trunk_dir * 4.0
	# Horizontal look back at the trunk, tilted 15 deg down (tan 15 deg ~= 0.268).
	var look := (-trunk_dir + Vector3(0.0, -0.268, 0.0)).normalized()

	world.set_effect_enabled("brick_cache", false)
	var off := world.hooks().debug_raymarch_normal_probe(eye, look, 160, 120)
	world.set_effect_enabled("brick_cache", true)
	var on := world.hooks().debug_raymarch_normal_probe(eye, look, 160, 120)
	assert_bool(off["ran"]).is_true()
	assert_bool(on["ran"]).is_true()
	assert_int(on["hits"]).is_greater(3000)
	# The cache path must actually be in play, or the bound below passes vacuously.
	assert_int(world.hooks().debug_brick_cache_flags(Vector3i((trunk / 0.8).floor())) & 1).is_equal(1)
	assert_float(on["rms_ndl"]).is_less_equal(0.03)
