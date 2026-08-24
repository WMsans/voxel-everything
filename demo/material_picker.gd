extends Control
# Material picker for the demo shell (M).
#
# The demo's tools (demo/edit_tool.gd) answer HOW terrain changes -- carve, fill, paint,
# drill, on keys 1-4. This answers WITH WHAT. It deliberately does not touch tool selection
# or the radius wheel; those bindings stay exactly where they were.
#
# It shows each material's hardness and glow next to its swatch because those numbers are
# the material system: a picker that showed only names would demonstrate nothing that a
# hardcoded fill_material export did not already do.

@export var world_path: NodePath
@export var tool_path: NodePath
@export var toggle_key := KEY_M

var _world: VoxelWorld
var _tool: Node
var _materials: Array = []
var _buttons: Array[Button] = []
var _selected := 0
var _mouse_mode_before := Input.MOUSE_MODE_CAPTURED

@onready var _grid: GridContainer = %Grid
@onready var _detail: Label = %Detail

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	visible = false
	_world = get_node_or_null(world_path) as VoxelWorld
	_tool = get_node_or_null(tool_path)
	if _world:
		_materials = _world.material_table()
	_populate()
	select(0)

func entry_count() -> int:
	return _materials.size()

func selected_id() -> int:
	if _selected < 0 or _selected >= _materials.size():
		return 0
	return int(_materials[_selected]["id"])

func is_open() -> bool:
	return visible

# The swatch. assets/materials/ carries a .gdignore so Godot never imports these PNGs --
# load() would return null. Reading the file straight off disk sidesteps the import
# pipeline entirely rather than pulling twenty textures into it for four swatches.
func _swatch(asset: String) -> ImageTexture:
	var path := ProjectSettings.globalize_path(
		"res://assets/materials/%s_basecolor.png" % asset)
	var img := Image.load_from_file(path)
	if img == null:
		return null
	img.resize(64, 64, Image.INTERPOLATE_BILINEAR)
	return ImageTexture.create_from_image(img)

func _populate() -> void:
	for b in _buttons:
		b.queue_free()
	_buttons.clear()
	for i in range(_materials.size()):
		var m: Dictionary = _materials[i]
		var b := Button.new()
		b.custom_minimum_size = Vector2(96, 112)
		b.expand_icon = true
		b.icon = _swatch(str(m["asset"]))
		b.text = str(m["name"])
		b.toggle_mode = true
		b.pressed.connect(select.bind(i))
		_grid.add_child(b)
		_buttons.append(b)

func select(index: int) -> void:
	if index < 0 or index >= _materials.size():
		return
	_selected = index
	for i in range(_buttons.size()):
		_buttons[i].button_pressed = i == index
	var m: Dictionary = _materials[index]
	_detail.text = "%s   hardness %.2f   glow %.2f" % [
		m["name"], float(m["hardness"]), float(m["glow"])]
	# Both, deliberately: Fill and Paint should never disagree about what the picker says
	# is selected. The engine wants the material ID, which is the array index plus one --
	# writing the index here would silently place the wrong material.
	if _tool:
		_tool.fill_material = int(m["id"])
		_tool.paint_material = int(m["id"])

func open() -> void:
	if visible:
		return
	_mouse_mode_before = Input.mouse_mode
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	visible = true

func close() -> void:
	if not visible:
		return
	visible = false
	Input.mouse_mode = _mouse_mode_before

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if event.keycode == toggle_key:
		close() if visible else open()
		get_viewport().set_input_as_handled()
	elif event.keycode == KEY_ESCAPE and visible:
		close()
		get_viewport().set_input_as_handled()
