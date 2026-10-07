extends GdUnitTestSuite

const CreateWorld := preload("res://demo/scripts/create_world.gd")
const TYPES_DIR := "res://demo/world_types"
const FIXTURE_DIR := "user://test_world_types_fixture"

var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

func test_seed_text_parses_like_minecraft() -> void:
	assert_int(CreateWorld.parse_seed("42")).is_equal(42)
	assert_int(CreateWorld.parse_seed(" 42 ")).is_equal(42)
	assert_int(CreateWorld.parse_seed("+42")).is_equal(42)
	assert_int(CreateWorld.parse_seed("0")).is_equal(0)
	assert_int(CreateWorld.parse_seed("-0")).is_equal(0)
	assert_int(CreateWorld.parse_seed("-1")).is_equal(0xffffffff)
	assert_int(CreateWorld.parse_seed("hello")).is_equal("hello".hash() & 0xffffffff)
	assert_int(CreateWorld.parse_seed("🌲")).is_equal("🌲".hash() & 0xffffffff)

func test_an_overlong_number_is_hashed_not_overflowed() -> void:
	var t := "99999999999999999999"
	assert_int(CreateWorld.parse_seed(t)).is_equal(t.hash() & 0xffffffff)

func test_a_blank_seed_is_random_and_never_zero() -> void:
	for i in range(50):
		var s: int = CreateWorld.parse_seed("   ")
		assert_int(s).is_greater(0)
		assert_int(s).is_less_equal(0xffffffff)

func test_world_types_list_default_mesas_flat() -> void:
	var names: Array = CreateWorld.load_types(TYPES_DIR).map(func(t): return t.display_name)
	assert_array(names).is_equal(["Default", "Mesas", "Flat"])

func test_a_stray_resource_in_the_types_folder_is_skipped() -> void:
	DirAccess.make_dir_recursive_absolute(FIXTURE_DIR)
	var wt := WorldType.new()
	wt.display_name = "Real"
	wt.pipeline_path = "res://assets/pipelines/default.pipeline"
	ResourceSaver.save(wt, FIXTURE_DIR.path_join("a_real.tres"))
	ResourceSaver.save(Theme.new(), FIXTURE_DIR.path_join("b_stray.tres"))
	var types: Array = CreateWorld.load_types(FIXTURE_DIR)
	assert_int(types.size()).is_equal(1)
	assert_str(types[0].display_name).is_equal("Real")
	for f in DirAccess.get_files_at(FIXTURE_DIR):
		DirAccess.remove_absolute(FIXTURE_DIR.path_join(f))

func test_a_missing_types_folder_lists_nothing() -> void:
	assert_int(CreateWorld.load_types("res://no/such/dir").size()).is_equal(0)

func test_create_builds_the_chosen_world_before_it_enters_the_tree() -> void:
	var mesas: WorldType = CreateWorld.load_types(TYPES_DIR)[1]
	var main: Node = CreateWorld.build_world(mesas, 4242)
	_nodes.append(main)
	assert_bool(main.is_inside_tree()).is_false()
	var w: VoxelWorldScene = main.get_node("VoxelWorld")
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/mesas.pipeline")
	assert_int(w.world_seed).is_equal(4242)
	assert_object(main.get_node_or_null("GroundSpawn")).is_not_null()

func test_the_menu_background_texture_loads() -> void:
	for path in ["res://demo/scenes/create_world.tscn", "res://demo/scenes/title.tscn"]:
		var screen: Control = load(path).instantiate()
		add_child(screen)
		_nodes.append(screen)
		assert_object(screen.get_node("Background").texture).is_not_null()

func test_the_type_button_cycles_and_enter_creates() -> void:
	var screen = load("res://demo/scenes/create_world.tscn").instantiate() # untyped: launch is a script member
	var launched: Array = []
	screen.launch = func(world: Node) -> void: launched.append(world)
	add_child(screen)
	_nodes.append(screen)
	var button: Button = screen.get_node("%WorldType")
	assert_str(button.text).is_equal("World Type: Default")
	button.pressed.emit()
	assert_str(button.text).is_equal("World Type: Mesas")
	button.pressed.emit()
	assert_str(button.text).is_equal("World Type: Flat")
	button.pressed.emit()
	assert_str(button.text).is_equal("World Type: Default")
	button.pressed.emit()
	var seed_box: LineEdit = screen.get_node("%Seed")
	seed_box.text = "777"
	seed_box.text_submitted.emit(seed_box.text)
	assert_int(launched.size()).is_equal(1)
	var w: VoxelWorldScene = launched[0].get_node("VoxelWorld")
	assert_str(w.world_type.display_name).is_equal("Mesas")
	assert_int(w.world_seed).is_equal(777)
	_nodes.append(launched[0])
