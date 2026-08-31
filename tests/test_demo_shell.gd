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

func test_demo_carve_uses_the_center_ray_material() -> void:
	var t := make_tool()
	await get_tree().process_frame
	var w: VoxelWorld = t.get_node("../World")
	var cam: Camera3D = t.get_node("../Cam")
	w.ensure_initialized()

	var hit := {}
	for x in range(20, 60):
		for z in range(20, 60):
			var candidate: Dictionary = w.hooks().debug_raycast(
				Vector3(x, 80, z), Vector3.DOWN)
			if candidate.get("material", 0) == 2: # rock hardness 3
				hit = candidate
				break
		if not hit.is_empty():
			break
	assert_bool(hit.is_empty()).override_failure_message(
		"the fixture could not find a rock surface for the demo ray").is_false()

	var hp: Vector3 = hit["pos"]
	cam.global_transform = Transform3D(
		Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(hp.x, 80.0, hp.z))
	t.set("radius", 3.0)
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	var old_mode := Input.mouse_mode
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	t._unhandled_input(click)
	Input.mouse_mode = old_mode

	for i in range(12):
		w.hooks().debug_stream_frame(Vector3(hp.x, hp.y + 5.0, hp.z))
	var region := Vector3i(floori(hp.x / 25.6), floori(hp.y / 25.6), floori(hp.z / 25.6))
	var slot: int = w.hooks().debug_region_map_entry(region)
	assert_int(slot).is_greater_equal(0)
	var rd := w.hooks().debug_local_rd() as RenderingDevice
	var bytes := rd.buffer_get_data(w.hooks().debug_op_pool(), slot * 256 * 32, 32)
	assert_float(bytes.to_float32_array()[5]).override_failure_message(
		"the demo did not scale its nominal radius by the center ray's rock hardness"
		).is_equal_approx(1.0, 0.0001)

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

func test_the_demo_has_a_material_picker_wired_to_the_edit_tool() -> void:
	var root: Node = load("res://demo/main.tscn").instantiate()
	# The scene's exports use absolute paths (/root/Main/...), so the instance only
	# resolves its world_path/tool_path if it sits at exactly that spot in the tree.
	root.name = "Main"
	get_tree().root.add_child(root)
	_roots.append(root)
	var picker: Control = root.get_node_or_null("HUD/MaterialPicker")
	assert_object(picker).override_failure_message(
		"main.tscn has no HUD/MaterialPicker").is_not_null()
	assert_bool(picker.tool_path.is_empty()).override_failure_message(
		"the picker has no tool_path: selecting a material would change nothing"
		).is_false()
	assert_object(picker.get_node_or_null(picker.tool_path)).override_failure_message(
		"the picker's tool_path does not resolve").is_not_null()

func test_help_lists_every_binding_the_demo_uses() -> void:
	var help := Control.new()
	help.set_script(HELP_SCRIPT)
	add_child(help)
	_roots.append(help)
	await get_tree().process_frame
	var text: String = help.help_text()
	for key in ["W", "F", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F12", "P", "1", "4", "Wheel"]:
		assert_str(text).contains(key)
