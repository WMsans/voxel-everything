extends Node
# Spec §8's two dev-build affordances: a shader hot-reload keybind, and a differential
# self-check that runs the CPU and GPU paths and diffs them. Also owns the F12 screenshot
# and the F3 cost-view toggle so every key in the help overlay has a handler.

@export var world_path: NodePath

var _world: VoxelWorld

func _ready() -> void:
	_world = get_node_or_null(world_path) as VoxelWorld

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo or _world == null:
		return
	match event.keycode:
		KEY_F3:
			var enabled := _world.get_effect_enabled("cost_view")
			_world.set_effect_enabled("cost_view", not enabled)
			print("cost view %s" % ("on" if not enabled else "off"))
			get_viewport().set_input_as_handled()
		KEY_F5:
			_world.request_shader_reload()
			print("shader reload requested")
			get_viewport().set_input_as_handled()
		KEY_F6:
			var d: Dictionary = _world.hooks().debug_self_check()
			print("SELF-CHECK ok=%s field=%d brick=%d mesh=%d lod=%d occ=%d (%.1f ms)" % [
				str(d["ok"]), d["field_mismatches"], d["brick_mismatches"],
				d["mesh_mismatches"], d["lod_mismatches"], d["occupancy_mismatches"],
				d["elapsed_ms"]])
			get_viewport().set_input_as_handled()
		KEY_F12:
			var image := get_viewport().get_texture().get_image()
			var path := "user://shot_%d.png" % Time.get_ticks_msec()
			image.save_png(path)
			print("screenshot: %s" % ProjectSettings.globalize_path(path))
			get_viewport().set_input_as_handled()
