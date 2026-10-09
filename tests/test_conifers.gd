extends GdUnitTestSuite

# Fjords conifers (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md). The stage half
# reads the CPU field through VoxelWorld.raycast; the pass half (Tasks 6-7) reads what the
# shipping passes wrote through debug_conifer_stats.
const FJORDS := "res://assets/pipelines/fjords.pipeline"
const WATER_Y := 51.2
# fjords.pipeline: shore 2 (+1), snow line 380 - jitter 25 - ridge drop 40 - margin 40.
const SHORE_E := 3.0
const TREELINE_E := 275.0
# A disc reaches 1.15 x 1.25 x 3 m from its foot; on a slope-1.7 wall that is 7.3 m of height.
const DISC_DROP := 8.0

var _world: VoxelWorld

func after_test() -> void:
	if is_instance_valid(_world):
		_world.free()

func _open(seed := 0) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = FJORDS
	_world.world_seed = seed
	_world.use_local_device = true
	_world.physics_enabled = false
	_world.stream_radius_m = 2600.0
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	assert_int(_world.hooks().debug_pump_sectors(Vector3.ZERO, 1600.0, 900)).is_greater(0)

func _material(name: String) -> int:
	var names: Array = _world.material_table().map(func(m): return m["name"])
	return names.find(name) + 1

# Forest hits on a 61 x 61 grid at 25 m over +-750 m.
func _forest_hits() -> Array:
	var forest := _material("forest")
	var out := []
	for i in range(-30, 31):
		for j in range(-30, 31):
			var hit: Dictionary = _world.raycast(Vector3(i * 25.0, 800.0, j * 25.0), Vector3.DOWN, 1600.0)
			if hit["hit"] and int(hit["material"]) == forest:
				out.append(hit["pos"])
	return out

func test_the_forest_floor_appears_on_the_walls() -> void:
	_open()
	var hits := _forest_hits()
	assert_int(hits.size()).override_failure_message("no forest material in a 1.5 km grid").is_greater(20)

# Spec §4.1: no tree stands in the shore band or above the tree line. A disc spreads from its
# foot by up to DISC_DROP metres of height on the steepest permitted wall.
func test_the_forest_floor_stays_between_shore_and_tree_line() -> void:
	_open()
	for p in _forest_hits():
		var e: float = p.y - WATER_Y
		assert_float(e).override_failure_message("forest at e=%.1f, %s" % [e, p]).is_greater(SHORE_E - DISC_DROP)
		assert_float(e).override_failure_message("forest at e=%.1f, %s" % [e, p]).is_less(TREELINE_E + DISC_DROP)

# Trunks are bark and stand up out of the forest floor: a horizontal ray at 6 m above a
# forest hit, cast across its cell, meets bark somewhere in the grid.
func test_trunks_are_bark() -> void:
	_open()
	var bark := _material("bark")
	var seen := 0
	for p in _forest_hits():
		for dir in [Vector3.RIGHT, Vector3.LEFT, Vector3.FORWARD, Vector3.BACK]:
			var hit: Dictionary = _world.raycast(p + Vector3(0.0, 6.0, 0.0) - dir * 6.0, dir, 12.0)
			if hit["hit"] and int(hit["material"]) == bark:
				seen += 1
	assert_int(seen).override_failure_message("no ray met a trunk near any forest floor").is_greater(0)
