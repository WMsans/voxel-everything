extends GdUnitTestSuite
# Pins what the demo scene wires together, by node TYPE and by what each path RESOLVES to --
# never by the path text. The world-types plan moves nodes under a reusable world scene;
# this suite is how it proves the wiring survived the move.

const MAIN := "res://demo/main.tscn"

var _root: Node

func after_test() -> void:
	if is_instance_valid(_root):
		_root.free()
	_root = null

func _open() -> Node:
	_root = load(MAIN).instantiate()
	# The scene's absolute /root/Main/... paths only resolve at exactly this spot.
	_root.name = "Main"
	get_tree().root.add_child(_root)
	return _root

func _world() -> VoxelWorld:
	return _root.get_node("VoxelWorld") as VoxelWorld

# owned = false: once the world is an instanced sub-scene its children are owned by it, not
# by Main, and find_children's default would skip them.
func _one(type: String) -> Node:
	var found := _root.find_children("*", type, true, false)
	assert_int(found.size()).override_failure_message(
		"expected exactly one %s in main.tscn, found %d" % [type, found.size()]).is_equal(1)
	return found[0]

func test_both_compositor_effects_point_at_the_world() -> void:
	_open()
	var we := _one("WorldEnvironment") as WorldEnvironment
	var effects: Array = we.compositor.compositor_effects
	assert_int(effects.size()).is_equal(2)
	for e in effects:
		assert_object(get_tree().root.get_node_or_null(e.world_path)).override_failure_message(
			"%s.world_path does not resolve to the world" % e.get_class()).is_same(_world())

func test_the_sun_path_resolves_to_a_directional_light() -> void:
	_open()
	var w := _world()
	assert_object(w.get_node_or_null(w.sun_light_path)).is_instanceof(DirectionalLight3D)

func test_settings_drive_the_world_and_the_menu_drives_the_settings() -> void:
	_open()
	var vs := _one("VoxelSettings") as VoxelSettings
	assert_object(vs.get_node_or_null(vs.world_path)).is_same(_world())
	var menu = _root.get_node("HUD/SettingsMenu") # untyped: settings_path is a script property
	assert_object(menu.get_node_or_null(menu.settings_path)).is_same(vs)

func test_the_demo_layer_reaches_the_world() -> void:
	_open()
	var w := _world()
	# Untyped: these exports are script properties, which a native-typed variable cannot see.
	var hud = _root.get_node("HUD/Label")
	assert_object(hud.get_node_or_null(hud.world_path)).is_same(w)
	var tool = _root.get_node("EditTool")
	assert_object(tool.get_node_or_null(tool.world_path)).is_same(w)
	assert_object(tool.get_node_or_null(tool.camera_path)).is_same(_root.get_node("Player/Camera3D"))
	var picker = _root.get_node("HUD/MaterialPicker")
	assert_object(picker.get_node_or_null(picker.world_path)).is_same(w)
	var dev = _root.get_node("DevTools")
	assert_object(dev.get_node_or_null(dev.world_path)).is_same(w)
	assert_object(w.get_node_or_null(w.physics_center_path)).is_same(_root.get_node("Player"))

func test_the_shipped_values_the_benchmark_rides_on() -> void:
	_open()
	var w := _world()
	assert_float(w.near_field_scale).is_equal_approx(0.4, 1e-6)
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/default.pipeline")
	assert_int(w.process_mode).is_equal(Node.PROCESS_MODE_ALWAYS)
	assert_vector((_root.get_node("Player") as Node3D).position).is_equal(Vector3(8, 62, 8))
	var env: Environment = (_one("WorldEnvironment") as WorldEnvironment).environment
	assert_bool(env.glow_enabled).is_true()
	assert_float(env.glow_hdr_threshold).is_equal_approx(1.05, 1e-6)
	assert_float(env.ambient_light_energy).is_equal_approx(0.4, 1e-6)
