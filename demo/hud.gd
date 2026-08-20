extends Label

@export var world_path: NodePath
var _world: VoxelWorld
var _frames := 0

func _ready() -> void:
	if not world_path.is_empty():
		_world = get_node(world_path)

func _process(_delta: float) -> void:
	_frames += 1
	if _frames % 15 != 0:
		return # streaming stats read back GPU counters; don't stall every frame
	var fps := Engine.get_frames_per_second()
	var ms := 1000.0 / maxf(float(fps), 0.001)
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
	text = "%d fps  (%.1f ms)  |  %s%s%s%s\n%s" % [fps, ms, s, p, isl, lod, gpu_line]
