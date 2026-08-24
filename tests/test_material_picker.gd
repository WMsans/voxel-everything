extends GdUnitTestSuite

var _roots: Array = []

func after_test() -> void:
	for r in _roots:
		if is_instance_valid(r):
			r.free()
	_roots.clear()

# A stand-in for demo/edit_tool.gd: the picker only needs the two properties it writes, and
# a real EditTool would demand a VoxelWorld and a Camera3D for no benefit here.
class FakeTool:
	extends Node
	var fill_material := 4
	var paint_material := 1

func make_picker() -> Control:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = true
	world.physics_enabled = false
	var root := Node.new()
	root.add_child(world)
	world.name = "VoxelWorld"
	var tool := FakeTool.new()
	tool.name = "EditTool"
	root.add_child(tool)
	var picker: Control = load("res://demo/material_picker.tscn").instantiate()
	picker.world_path = NodePath("../VoxelWorld")
	picker.tool_path = NodePath("../EditTool")
	root.add_child(picker)
	add_child(root)
	_roots.append(root)
	return picker

func test_it_lists_every_material() -> void:
	var p := make_picker()
	var world: VoxelWorld = p.get_node(p.world_path)
	assert_int(p.entry_count()).is_equal(world.material_table().size())

# The failure this guards is silent and expensive: writing the ARRAY INDEX where the engine
# expects a material ID paints every material one slot off, with no error anywhere.
func test_selecting_writes_the_material_id_not_the_index() -> void:
	var p := make_picker()
	var world: VoxelWorld = p.get_node(p.world_path)
	var tool = p.get_node(p.tool_path)
	p.select(1)
	var expected := int(world.material_table()[1]["id"])
	assert_int(p.selected_id()).is_equal(expected)
	assert_int(tool.fill_material).is_equal(expected)
	assert_int(tool.paint_material).is_equal(expected)

func test_it_starts_closed_and_toggles() -> void:
	var p := make_picker()
	assert_bool(p.is_open()).is_false()
	p.open()
	assert_bool(p.is_open()).is_true()
	p.close()
	assert_bool(p.is_open()).is_false()

# The demo runs with the mouse captured. A panel that released capture and never restored it
# would leave the player unable to look around after choosing a material.
func test_closing_restores_the_previous_mouse_mode() -> void:
	var p := make_picker()
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	p.open()
	assert_int(Input.mouse_mode).is_equal(Input.MOUSE_MODE_VISIBLE)
	p.close()
	assert_int(Input.mouse_mode).is_equal(Input.MOUSE_MODE_CAPTURED)

func test_an_out_of_range_selection_is_ignored() -> void:
	var p := make_picker()
	p.select(0)
	var before: int = p.selected_id()
	p.select(9999)
	p.select(-1)
	assert_int(p.selected_id()).is_equal(before)
