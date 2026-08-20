extends Node
# Portfolio capture (spec §8: the benchmark scene "doubles as the portfolio capture rig").
#
#   godot --path . --resolution 2560x1440 demo/main.tscn -- --capture
#
# The camera path is a deterministic function of the FRAME INDEX, and edits are scheduled by
# frame, so the run writes the same 900 camera frames whether the engine manages 90 fps or 12.
# Physics/destruction simulation determinism is not proven; a slow frame changes the wall-clock
# length of the capture and may also change any frame-rate-dependent simulation outcome.
# tools/encode_capture.sh turns them into a video.

const FRAMES := 900
const WARMUP := 90        # let the near field and the first LoD chunks land before frame 0
const OUT_DIR := "user://capture"

var _active := false
var _frame := -WARMUP
var _world: VoxelWorld
var _player: CharacterBody3D
var _cam: Camera3D
var _tool: VoxelEditTool

func _ready() -> void:
	if not ("--capture" in OS.get_cmdline_user_args()):
		return
	_active = true
	Engine.max_fps = 0
	_world = get_parent().get_node("VoxelWorld")
	_player = get_parent().get_node("Player")
	_cam = _player.get_node("Camera3D")
	_player.set_physics_process(false)
	_player.set_process_unhandled_input(false)
	get_parent().get_node("HUD/Label").set_mode(2)   # hidden
	get_parent().get_node("HUD/Help").visible = false
	_tool = ClassDB.instantiate("VoxelEditTool")
	_world.add_child(_tool)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT_DIR))

# Mirror of extension/src/generator/generator.cpp and shaders/field.glslh, as
# demo/benchmark.gd already keeps one. Used only to hold the path above the ground.
func terrain_height(x: float, z: float) -> float:
	return 51.2 + (
			6.0 * sin(x * 0.11) * cos(z * 0.13)
			+ 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043)
			+ 1.0 * sin(x * 0.23 + z * 0.19))

func camera_at(frame: int) -> Transform3D:
	# A slow arc across a ridge, dropping toward the ground as it goes: the near field fills
	# the screen at the end, the far field carries the start, and the seam crosses the middle
	# of the shot -- which is the thing this engine is for.
	var t := float(clampi(frame, 0, FRAMES)) / float(FRAMES)
	var angle := 0.9 + t * 1.1
	var radius := 120.0 - t * 85.0
	var cx := 480.0
	var cz := 340.0
	var p := Vector3(cx + cos(angle) * radius, 0.0, cz + sin(angle) * radius)
	p.y = terrain_height(p.x, p.z) + 26.0 - t * 18.0
	var look := Vector3(cx, terrain_height(cx, cz) + 2.0, cz)
	return Transform3D(Basis.looking_at(look - p, Vector3.UP), p)

func events_at(frame: int) -> Array:
	# The canned destruction: four blasts on the ridge line, spaced so each one's island has
	# time to fall, sleep and re-merge before the next.
	match frame:
		240: return [["subtract", Vector3(480.0, terrain_height(480.0, 340.0) + 1.0, 340.0), 5.0]]
		380: return [["subtract", Vector3(474.0, terrain_height(474.0, 346.0) + 2.0, 346.0), 4.0]]
		520:
			# The drill is expanded here into its ten sphere-subtract operations so the
			# scheduled-script count matches what _process actually applies.
			var events: Array = []
			for i in range(10):
				events.append(["subtract",
					Vector3(486.0, terrain_height(486.0, 334.0) + 3.0, 334.0)
						+ Vector3.DOWN * (0.6 * i), 1.0])
			return events
		660: return [["subtract", Vector3(480.0, terrain_height(480.0, 340.0) + 6.0, 340.0), 6.0]]
		_: return []

func _process(_delta: float) -> void:
	if not _active:
		return
	_frame += 1
	if _frame < 0:
		return                    # warmup: stream, do not record
	if _frame >= FRAMES:
		print("CAPTURE done: %d frames in %s" % [FRAMES, ProjectSettings.globalize_path(OUT_DIR)])
		_world.shutdown_render_resources()
		set_process(false)
		get_tree().create_timer(5.0).timeout.connect(get_tree().quit)
		return

	var xform := camera_at(_frame)
	_player.global_position = xform.origin
	_cam.global_transform = xform
	for e in events_at(_frame):
		_tool.apply_sphere_subtract(e[1], e[2])

	# The image has to be read AFTER the frame it belongs to has been drawn.
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	image.save_png("%s/frame_%05d.png" % [OUT_DIR, _frame])
