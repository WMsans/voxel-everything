extends Node
# Demo destruction tool (spec §5 "demo edit tools"): 1 Carve, 2 Fill, 3 Paint, 4 Drill.
# Aims with the world's analytic raycast — the same field the GPU bricks are generated
# from — so the reticle tracks the surface exactly, with no physics and no readback stall.

enum Tool { SUBTRACT, ADD, PAINT, DRILL }

const TOOL_NAMES := ["Carve", "Fill", "Paint", "Drill"]
const RADIUS_MIN := 0.5
const RADIUS_MAX := 8.0
const RADIUS_STEP := 1.25

@export var world_path: NodePath
@export var camera_path: NodePath
@export var player_path: NodePath
@export var radius := 3.0
@export var fill_material := 4
@export var paint_material := 1
@export var drill_radius := 0.6
@export var drill_length := 6.0
@export var drill_steps := 10

var active_tool := Tool.SUBTRACT

var _world: VoxelWorld
var _tool: VoxelEditTool
var _cam: Camera3D
var _player: CharacterBody3D

func _ready() -> void:
	_world = get_node(world_path)
	_cam = get_node(camera_path)
	if not player_path.is_empty():
		_player = get_node(player_path)
	_tool = ClassDB.instantiate("VoxelEditTool")
	_world.add_child(_tool) # VoxelEditTool resolves the world through its parent

# Split out of _unhandled_input so a test can drive it without a viewport, and so the two
# input paths (key event, wheel event) each have exactly one implementation.
func select_tool_from_key(event: InputEventKey) -> bool:
	if not event.pressed or event.echo:
		return false
	var index := event.keycode - KEY_1
	if index < 0 or index >= TOOL_NAMES.size():
		return false
	active_tool = index
	return true

func adjust_radius(notches: int) -> void:
	radius = clampf(radius * pow(RADIUS_STEP, float(notches)), RADIUS_MIN, RADIUS_MAX)

func tool_name() -> String:
	return TOOL_NAMES[active_tool]

func _unhandled_input(event: InputEvent) -> void:
	if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		return
	if event is InputEventKey:
		if event.pressed and not event.echo:
			if select_tool_from_key(event):
				get_viewport().set_input_as_handled()
				return
			if event.keycode == KEY_R:
				_drill()
				get_viewport().set_input_as_handled()
				return
		return
	var mb := event as InputEventMouseButton
	if mb == null or not mb.pressed:
		return
	if mb.button_index == MOUSE_BUTTON_WHEEL_UP:
		adjust_radius(1)
		return
	if mb.button_index == MOUSE_BUTTON_WHEEL_DOWN:
		adjust_radius(-1)
		return
	var hit: Dictionary = _world.hooks().debug_raycast(
		_cam.global_position, -_cam.global_transform.basis.z)
	if not hit["hit"]:
		return
	var pos: Vector3 = hit["pos"]
	match mb.button_index:
		MOUSE_BUTTON_LEFT:
			match active_tool:
				Tool.SUBTRACT:
					_tool.apply_sphere_subtract(pos, radius)
					_kick(pos)
				Tool.ADD:
					_tool.apply_sphere_add(pos, radius * 0.7, fill_material)
				Tool.PAINT:
					_tool.apply_sphere_paint(pos, radius, paint_material)
				Tool.DRILL:
					_drill()
		MOUSE_BUTTON_RIGHT:
			_tool.apply_sphere_add(pos, radius * 0.7, fill_material)
		MOUSE_BUTTON_MIDDLE:
			_tool.apply_sphere_paint(pos, radius, paint_material)

func _drill() -> void:
	# Spec section 5's line drill: a row of small subtracts along the aim ray, starting at
	# the surface and boring inward. It is the tool that actually severs an overhang, because
	# a narrow bore cuts a support without swallowing the piece it was holding up.
	var hit: Dictionary = _world.hooks().debug_raycast(
		_cam.global_position, -_cam.global_transform.basis.z)
	if not hit["hit"]:
		return
	var dir := -_cam.global_transform.basis.z.normalized()
	var start: Vector3 = hit["pos"]
	var step := drill_length / float(maxi(drill_steps - 1, 1))
	for i in range(drill_steps):
		_tool.apply_sphere_subtract(start + dir * (step * i), drill_radius)
	_kick(start)

func _kick(pos: Vector3) -> void:
	if _player == null or _player.flying:
		return
	var away: Vector3 = _player.global_position - pos
	var d := away.length()
	var reach := radius * 3.0
	if d > reach or d < 0.001:
		return
	_player._impulse += away.normalized() * (1.0 - d / reach) * 14.0
