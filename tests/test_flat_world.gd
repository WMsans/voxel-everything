extends GdUnitTestSuite
# The Flat world type (spec §5.3): a level grass plain at SURFACE_Y + 2 m, and no canopy pass,
# because it has no trees stage for one to stand on.

const SURFACE_Y := 51.2
const GRASS := 1 # ve::kMaterials index 0 + 1

var _world: VoxelWorld

func after_test() -> void:
	if is_instance_valid(_world):
		_world.free()

func _open() -> VoxelWorld:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = "res://assets/pipelines/flat.pipeline"
	_world.use_local_device = true
	_world.physics_enabled = false
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	return _world

func test_flat_is_level_grass_at_two_metres() -> void:
	var w := _open()
	for xz in [Vector2(8, 8), Vector2(-300, 120), Vector2(2500, -4000)]:
		var hit: Dictionary = w.raycast(Vector3(xz.x, 600.0, xz.y), Vector3.DOWN, 1200.0)
		assert_bool(hit["hit"]).is_true()
		assert_float((hit["pos"] as Vector3).y).is_equal_approx(SURFACE_Y + 2.0, 0.05)
		assert_int(int(hit["material"])).is_equal(GRASS)

func test_the_seed_does_not_change_flat() -> void:
	var w := _open()
	var a: Vector2 = w.hooks().debug_eval_field(Vector3(8, 50, 8), PackedByteArray(), 0)
	w.free()
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = "res://assets/pipelines/flat.pipeline"
	_world.world_seed = 4242
	_world.use_local_device = true
	_world.physics_enabled = false
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	var b: Vector2 = _world.hooks().debug_eval_field(Vector3(8, 50, 8), PackedByteArray(), 0)
	assert_vector(b).is_equal(a)

func test_a_world_without_trees_has_no_leaf_pass() -> void:
	var w := _open()
	assert_bool(w.is_initialized()).is_true()
	assert_bool(w.hooks().debug_leaf_stats()["ran"]).is_false()
