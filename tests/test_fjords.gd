extends GdUnitTestSuite

# The Fjords world (spec §6–§7). Reads the shipping bake's statistics and the CPU field.
const FJORDS := "res://assets/pipelines/fjords.pipeline"

var _world: VoxelWorld

func after_test() -> void:
	if is_instance_valid(_world):
		_world.free()

func _open(seed: int, radius: float) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = FJORDS
	_world.world_seed = seed
	_world.use_local_device = true
	_world.physics_enabled = false
	_world.stream_radius_m = radius
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()

# Spec §6.4 and Review Focus 5: over +-3 km at three seeds no sector is steeper than the
# field's declared bound assumes, and no texel clamps at either end of the encoded range.
func test_no_sector_breaks_the_slope_limit_or_the_encoded_range() -> void:
	for seed in [0, 7, 424242]:
		_open(seed, 4300.0)
		assert_int(_world.hooks().debug_pump_sectors(Vector3.ZERO, 3000.0, 900)).is_greater(0)
		var s: Dictionary = _world.hooks().debug_sector_stats()
		assert_int(s["over_limit"]).override_failure_message(
			"seed %d: %d sectors over the slope limit, worst %.2f" % [seed, s["over_limit"], s["max_slope"]]
			).is_equal(0)
		assert_float(s["r_min"]).override_failure_message("seed %d floor clamps" % seed).is_greater(0.01)
		assert_float(s["r_max"]).override_failure_message("seed %d peaks clamp" % seed).is_less(0.98)
		_world.free()

# The bands produce all four materials somewhere in a few kilometres (spec §7.2).
func test_the_bands_place_snow_rock_grass_and_shore() -> void:
	_open(0, 2600.0)
	assert_int(_world.hooks().debug_pump_sectors(Vector3.ZERO, 2000.0, 600)).is_greater(0)
	var seen := {}
	for i in range(-20, 21):
		for j in range(-20, 21):
			var hit: Dictionary = _world.raycast(Vector3(i * 100.0, 800.0, j * 100.0), Vector3.DOWN, 1600.0)
			if hit["hit"]:
				seen[int(hit["material"])] = true
	var names: Array = _world.material_table().map(func(m): return m["name"])
	for want in ["snow", "breakstone", "grass_01", "ground_01"]:
		assert_bool(seen.has(names.find(want) + 1)).override_failure_message(
			"no %s in a 4 km grid; seen ids %s" % [want, seen.keys()]).is_true()
