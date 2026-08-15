extends Node
# Demo destruction tool (spec §5 "demo edit tools"): LMB carves, RMB fills, MMB paints.
# Aims with the world's analytic raycast — the same field the GPU bricks are generated
# from — so the reticle tracks the surface exactly, with no physics and no readback stall.

@export var world_path: NodePath
@export var camera_path: NodePath
@export var player_path: NodePath
@export var radius := 3.0
@export var fill_material := 4
@export var paint_material := 1

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

func _unhandled_input(event: InputEvent) -> void:
	if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		return
	var mb := event as InputEventMouseButton
	if mb == null or not mb.pressed:
		return
	var hit: Dictionary = _world.debug_raycast(
		_cam.global_position, -_cam.global_transform.basis.z)
	if not hit["hit"]:
		return
	var pos: Vector3 = hit["pos"]
	match mb.button_index:
		MOUSE_BUTTON_LEFT:
			_tool.apply_sphere_subtract(pos, radius)
			_kick(pos)
		MOUSE_BUTTON_RIGHT:
			_tool.apply_sphere_add(pos, radius * 0.7, fill_material)
		MOUSE_BUTTON_MIDDLE:
			_tool.apply_sphere_paint(pos, radius, paint_material)

func _kick(pos: Vector3) -> void:
	if _player == null or _player.flying:
		return
	var away: Vector3 = _player.global_position - pos
	var d := away.length()
	var reach := radius * 3.0
	if d > reach or d < 0.001:
		return
	_player.velocity += away.normalized() * (1.0 - d / reach) * 14.0
