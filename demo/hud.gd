extends Label
# Demo HUD (spec §5 "demo edit tools" / M7 demo shell). Three detail modes:
#   0 full    — streaming/GPU counters plus the active tool
#   1 compact — one line: fps, frame ms, active tool and radius
#   2 hidden  — nothing, the mode the capture rig uses
# The reticle is a full-rect Control sibling under the same CanvasLayer; this script
# drives its draw signal so no second script file is needed.

enum Mode { FULL, COMPACT, HIDDEN }

@export var world_path: NodePath
@export var tool_path: NodePath
@export var reticle_path: NodePath

var mode := Mode.FULL

var _world: VoxelWorld
var _tool
var _reticle: Control
var _frames := 0

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	if not world_path.is_empty():
		_world = get_node(world_path)
	if not tool_path.is_empty():
		_tool = get_node_or_null(tool_path)
	if not reticle_path.is_empty():
		_reticle = get_node_or_null(reticle_path) as Control
	if _reticle == null:
		_reticle = get_parent().get_node_or_null("Reticle") as Control
	if _reticle:
		_reticle.mouse_filter = Control.MOUSE_FILTER_IGNORE
		_reticle.draw.connect(_draw_reticle)
	set_mode(mode)

func set_mode(value: int) -> void:
	mode = clampi(value, Mode.FULL, Mode.HIDDEN)
	var show := mode != Mode.HIDDEN
	visible = show
	if _reticle:
		_reticle.visible = show
	_update_text()

func _process(delta: float) -> void:
	if _reticle and mode != Mode.HIDDEN:
		_reticle.queue_redraw()
	_frames += 1
	if _frames % 15 != 0:
		return
	_update_text()

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F4:
		set_mode((mode + 1) % 3)
		get_viewport().set_input_as_handled()

func _update_text() -> void:
	if mode == Mode.HIDDEN:
		text = ""
		return
	var fps := Engine.get_frames_per_second()
	var ms := 1000.0 / maxf(float(fps), 0.001)
	if mode == Mode.COMPACT:
		var tool_name := "?"
		var radius := 0.0
		if _tool:
			tool_name = str(_tool.tool_name())
			radius = float(_tool.radius)
		text = "%d fps  (%.1f ms)  |  %s  radius %.1f" % [fps, ms, tool_name, radius]
		return

	var s := "world: booting"
	if _world and _world.is_initialized():
		var st: Dictionary = _world.debug_stream_stats()
		s = "regions %d  edits %d  ovf %d  ovr %d/%d  cons %d" % [
			st.get("resident_regions", 0), st.get("frame_edits", 0),
			st.get("overflow_ever", 0), st.get("override_bricks", 0),
			st.get("override_capacity", 0), st.get("consolidations", 0)]
	var p := ""
	var isl := ""
	var lod := ""
	if _world:
		var ph: Dictionary = _world.debug_physics_stats()
		# build_ms is the Jolt BVH build for the last chunk; it is the one physics number
		# that can show up in the frame time (spec section 6 budgets nothing for it, and the
		# streamer throttles it to shape_builds_per_frame).
		p = "  |  chunks %d (+%d)  bodies %d  build %.1fms" % [
			ph.get("chunks_resident", 0), ph.get("chunks_pending", 0),
			ph.get("bodies", 0), ph.get("build_ms", 0.0)]
		var st: Dictionary = _world.debug_island_stats()
		# islands/debris are what is in the air right now; spawned/merged are the running
		# totals, so a demo recording can be checked afterwards for whether the loop closed.
		isl = "  |  isl %d dbr %d (+%d/-%d)  cx %d  %.1fms" % [
			st.get("live_islands", 0), st.get("live_debris", 0),
			st.get("islands_spawned", 0), st.get("islands_merged", 0),
			st.get("connectivity_runs", 0), st.get("manager_ms", 0.0)]
		if _world.is_initialized():
			var ld: Dictionary = _world.debug_lod_stats()
			var pf: Dictionary = _world.debug_perf_stats()
			var pages_used: int = int(ld.get("pages_used", 0))
			var pages_total: int = int(ld.get("pages_total", 0))
			var culled: float = float(ld.get("culled_ratio", 0.0))
			lod = "  |  lod %dch %d/%dp %dpg %d%% %dbf %ddirty %.2fms" % [
				ld.get("chunks_resident", 0), pages_used, pages_total,
				ld.get("draw_pages", 0), int(round(culled * 100.0)),
				ld.get("builds_in_flight", 0), ld.get("dirty_chunks", 0),
				pf.get("lod_ms", 0.0)]
			lod = lod.replace(" %.2fms" % float(pf.get("lod_ms", 0.0)),
					" %.2fms cpu" % float(pf.get("lod_ms", 0.0)))
	var gpu_line := "GPU n/a"
	var gt: Dictionary = _world.debug_gpu_timings() if _world else {}
	if gt.get("valid", false):
		var shadows := maxf(float(gt.get("sun_shadow_gpu_ms", -1.0)), 0.0) + \
				maxf(float(gt.get("contact_gpu_ms", -1.0)), 0.0)
		gpu_line = "GPU ray %.2f lod %.2f ssgi %.2f ssr %.2f sh %.2f out %.2f ms" % [
			gt.get("raymarch_gpu_ms", -1.0), gt.get("lod_gpu_ms", -1.0),
			gt.get("ssgi_gpu_ms", -1.0), gt.get("ssr_gpu_ms", -1.0),
			shadows, gt.get("outlines_gpu_ms", -1.0)]
	var tool_line := ""
	if _tool:
		tool_line = "\n%s  radius %.1f" % [str(_tool.tool_name()), float(_tool.radius)]
	text = "%d fps  (%.1f ms)  |  %s%s%s%s\n%s%s" % [fps, ms, s, p, isl, lod, gpu_line, tool_line]

func _draw_reticle() -> void:
	if _reticle == null or mode == Mode.HIDDEN:
		return
	var center := _reticle.size * 0.5
	var color := Color(1.0, 1.0, 1.0, 0.9)
	var cross := 6.0
	_reticle.draw_line(center + Vector2(-cross, 0.0), center + Vector2(cross, 0.0), color, 1.0)
	_reticle.draw_line(center + Vector2(0.0, -cross), center + Vector2(0.0, cross), color, 1.0)
	var circle_radius := _reticle_circle_radius()
	if circle_radius > 0.0:
		_reticle.draw_arc(center, circle_radius, 0.0, TAU, 64, color, 1.0)

func _reticle_circle_radius() -> float:
	if _world == null or _tool == null:
		return 0.0
	var viewport := get_viewport()
	var cam: Camera3D = viewport.get_camera_3d() if viewport else null
	if cam == null:
		return 0.0
	var hit: Dictionary = _world.debug_raycast(
		cam.global_position, -cam.global_transform.basis.z)
	if not hit["hit"]:
		return 0.0
	var distance := float(hit["distance"])
	if distance < 0.1:
		return 0.0
	var world_radius := float(_tool.radius)
	if world_radius <= 0.0:
		return 0.0
	var viewport_size := viewport.get_visible_rect().size
	if viewport_size.y <= 0.0:
		return 0.0
	var half_fov_y := deg_to_rad(cam.fov) * 0.5
	return world_radius / distance * viewport_size.y / (2.0 * tan(half_fov_y))
