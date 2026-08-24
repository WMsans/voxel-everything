extends GdUnitTestSuite

const TOOL_SCRIPT := preload("res://demo/edit_tool.gd")
const HELP_SCRIPT := preload("res://demo/help.gd")
var _roots: Array = []

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()

func make_tool() -> Node:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	root.add_child(world)
	var cam := Camera3D.new()
	cam.name = "Cam"
	root.add_child(cam)
	var tool_node := Node.new()
	tool_node.name = "Tool"
	tool_node.set_script(TOOL_SCRIPT)
	tool_node.set("world_path", NodePath("../World"))
	tool_node.set("camera_path", NodePath("../Cam"))
	root.add_child(tool_node)
	return tool_node

func test_number_keys_select_tools() -> void:
	var t := make_tool()
	await get_tree().process_frame
	for i in range(4):
		var ev := InputEventKey.new()
		ev.keycode = KEY_1 + i
		ev.pressed = true
		t.select_tool_from_key(ev)
		assert_int(int(t.get("active_tool"))).is_equal(i)

func test_wheel_changes_radius_within_bounds() -> void:
	var t := make_tool()
	await get_tree().process_frame
	t.set("radius", 8.0)
	for i in range(10):
		t.adjust_radius(1)
	assert_float(float(t.get("radius"))).is_less_equal(8.0)
	for i in range(30):
		t.adjust_radius(-1)
	assert_float(float(t.get("radius"))).is_greater_equal(0.5)

# Emission above 1.0 only becomes visible bloom if the Environment asks for it. The engine
# writes HDR into the colour buffer before Godot's glow stage; this is the other half.
func test_the_demo_environment_has_glow_enabled() -> void:
	var scene: PackedScene = load("res://demo/main.tscn")
	var root: Node = scene.instantiate()
	var we: WorldEnvironment = root.get_node("WorldEnvironment")
	assert_bool(we.environment.glow_enabled).override_failure_message(
		"main.tscn's Environment has glow disabled: emissive materials will not bloom"
		).is_true()
	assert_float(we.environment.glow_hdr_threshold).is_greater(0.0)
	root.free()

func test_help_lists_every_binding_the_demo_uses() -> void:
	var help := Control.new()
	help.set_script(HELP_SCRIPT)
	add_child(help)
	_roots.append(help)
	await get_tree().process_frame
	var text: String = help.help_text()
	for key in ["W", "F", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F12", "P", "1", "4", "Wheel"]:
		assert_str(text).contains(key)
