extends GdUnitTestSuite

const Hud := preload("res://demo/scripts/hud.gd")

func test_the_line_names_the_seed_and_the_type() -> void:
	var w: VoxelWorldScene = load("res://demo/scenes/voxel_world.tscn").instantiate()
	w.world_type = load("res://demo/world_types/10_mesas.tres")
	w.world_seed = 42
	assert_str(Hud.world_line(w)).is_equal("seed 42 · Mesas")
	w.free()

func test_a_bare_world_falls_back_to_its_pipeline_name() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	assert_str(Hud.world_line(w)).is_equal("seed 0 · default")
	w.free()

func test_no_world_no_line() -> void:
	assert_str(Hud.world_line(null)).is_equal("")
