extends Node
# Frame-time harness. Runs only when a --benchmark* flag is passed after `--`.
#
#   --benchmark        steady state: the player is frozen, nothing streams after warmup.
#   --benchmark-move   the player flies forward continuously, so regions and collision
#                      chunks stream in for the whole run.
#   --benchmark-ridge  the second flythrough leg: low along a valley floor with a ridge
#                      between the camera and the far basin (the far-occludes-far case).
#   --benchmark-edit   the player is frozen and the edit tool fires every frame.
#   --benchmark-island the player is frozen and a sphere subtract severs a pillar every
#                      second, so connectivity, extraction, spawning and re-merging all run
#                      under the frame timer.
#
# The last two are the cases players actually complain about, and an AVERAGE hides them:
# a run that is 8 ms most of the time and 50 ms whenever a batch lands reads as "fine".
# So the report is percentiles plus the count of frames that missed 60 fps.

const WARMUP := 60
const FRAMES := 300
const ISLAND_FRAMES := 900
const TARGET_MS := 16.6
const GPU_DRAIN_FRAMES := 30
const MIN_GPU_SAMPLES := 30
const BUDGETS_MS := {
	"raymarch": 6.0, "lod": 2.0, "ssgi": 1.5, "ssr": 1.5,
	"shadows": 1.0, "outlines": 0.3, "frame": 16.0,
}

var _gpu_samples := {
	"raymarch": PackedFloat32Array(), "stream": PackedFloat32Array(), "lod": PackedFloat32Array(),
	"ssgi": PackedFloat32Array(), "ssr": PackedFloat32Array(),
	"shadows": PackedFloat32Array(), "outlines": PackedFloat32Array(),
	"unattributed": PackedFloat32Array(),
	"custom_frame": PackedFloat32Array(),
}
var _last_gpu_sample_id := -1
var _gpu_dropped_pairs := 0
var _gpu_timestamp_unit := "unavailable"
var _gpu_timestamp_scale := 0.0
var _gpu_timestamp_normalization := "unavailable"
var _gpu_timestamp_normalized := false
var _vsync_actual := "unknown"
var _draining := false
var _drain_frames := 0

var _mode := ""
var _frames := 0
var _samples: PackedFloat32Array = PackedFloat32Array()
var _perf_accum := {}
var _perf_max := {}
var _prev_perf := {}
var _prev_lod := {}
var _worst := {}
var _worst_ms := 0.0
var _lod_ms_samples: PackedFloat32Array = PackedFloat32Array()
var _draw_pages_samples: PackedFloat32Array = PackedFloat32Array()
var _culled_ratio_samples: PackedFloat32Array = PackedFloat32Array()
var _chunks_resident_samples: PackedFloat32Array = PackedFloat32Array()
var _pages_used_samples: PackedFloat32Array = PackedFloat32Array()
var _player: CharacterBody3D
var _world: VoxelWorld
var _tool: VoxelEditTool
var _cam: Camera3D
var _edit_phase := 0.0
var _island_timer := 0.0
var _island_built := false
var _island_waiting_for_merge := false
var _target_frames := FRAMES

func _effects_off_from_args(args: PackedStringArray) -> PackedStringArray:
	var effects := PackedStringArray()
	for arg in args:
		if not arg.begins_with("--effects-off="):
			continue
		for name in arg.trim_prefix("--effects-off=").split(",", false):
			var trimmed := String(name).strip_edges()
			if not trimmed.is_empty():
				effects.append(trimmed)
	return effects

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	for m in ["--benchmark-move", "--benchmark-ridge", "--benchmark-edit",
			"--benchmark-island", "--benchmark"]:
		if m in args:
			_mode = m
			break
	if _mode == "":
		return
	if _mode == "--benchmark-island":
		_target_frames = ISLAND_FRAMES
	# Without this the harness measures the DISPLAY, not the engine: a compositor that hands
	# an unfocused window one frame callback in eight reports a 133 ms frame and a 7 fps
	# "regression" that no code change can move.
	_record_vsync()

	_player = get_parent().get_node("Player")
	_world = get_parent().get_node("VoxelWorld")
	for effect in _effects_off_from_args(args):
		_world.set_effect_enabled(effect, false)
	_cam = _player.get_node("Camera3D")
	# Drive the player from here rather than from input, so a run is reproducible.
	_player.set_physics_process(false)
	_player.set_process_unhandled_input(false)
	if _mode == "--benchmark-ridge":
		# Low valley floor: (x,z) = (480,340) is a local low in the deterministic analytic
		# terrain, the (2,1) leg stays inside the world and close to the floor, and a ridge
		# rises ahead while the basin behind it drops away.
		var start := Vector3(480.0, _terrain_height(480.0, 340.0) + 1.5, 340.0)
		_player.global_transform = Transform3D(Basis.IDENTITY, start)
		_cam.transform = Transform3D(Basis.looking_at(Vector3(2.0, 0.0, 1.0).normalized(),
			Vector3(0.0, 1.0, 0.0)), Vector3(0.0, 0.7, 0.0))
	else:
		_player.global_transform = Transform3D(Basis.IDENTITY, Vector3(24, 63.2, 24))
		_cam.transform = Transform3D(Basis.looking_at(Vector3(6, -10, 6).normalized()),
			Vector3(0, 0.7, 0))
	if _mode == "--benchmark-edit" or _mode == "--benchmark-island":
		_tool = ClassDB.instantiate("VoxelEditTool")
		_world.add_child(_tool)

func _record_vsync() -> void:
	# Requesting DISABLED is not the same as getting it: a Wayland compositor can refuse,
	# and every frame percentile in M6 was qualified for exactly that reason. Ask, read
	# back, and print what the run actually measured.
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	match DisplayServer.window_get_vsync_mode():
		DisplayServer.VSYNC_DISABLED:
			_vsync_actual = "disabled"
		DisplayServer.VSYNC_ENABLED:
			_vsync_actual = "enabled"
		DisplayServer.VSYNC_ADAPTIVE:
			_vsync_actual = "adaptive"
		DisplayServer.VSYNC_MAILBOX:
			_vsync_actual = "mailbox"
		_:
			_vsync_actual = "unknown"

func _process(delta: float) -> void:
	if _mode == "":
		return
	if _draining:
		_capture_gpu_sample()
		_drain_frames += 1
		if _drain_frames >= GPU_DRAIN_FRAMES:
			_report()
			_world.shutdown_render_resources()
			set_process(false)
			# Let the renderer observe the render-thread cleanup before SceneTree teardown.
			get_tree().create_timer(5.0).timeout.connect(get_tree().quit)
		return
	_frames += 1

	if _mode == "--benchmark-move":
		# Straight line across the world at fly speed: a steady stream of new regions and
		# collision chunks, which is the "walking forward" case.
		_player.global_position += Vector3(0.7, 0.0, 0.7).normalized() * 25.0 * delta
	elif _mode == "--benchmark-ridge":
		# Follow the valley floor along (2,1), keeping the camera ~1.5 m above the analytic
		# terrain. The ridge stays ahead for the whole sampled leg.
		var p: Vector3 = _player.global_position + Vector3(2.0, 0.0, 1.0).normalized() * 25.0 * delta
		p.y = _terrain_height(p.x, p.z) + 1.5
		_player.global_position = p
	elif _mode == "--benchmark-edit" and _frames > WARMUP:
		_fire_edit()
	elif _mode == "--benchmark-island" and _frames > WARMUP:
		_island_cycle(delta)

	if _frames <= WARMUP:
		return # let the first regions land before sampling
	_capture_gpu_sample()
	var ms := delta * 1000.0
	_samples.append(ms)
	# `delta` is the PREVIOUS frame's duration, while debug_perf_stats() read now describes
	# the frame currently in progress. Pair each delta with the counters from the frame it
	# actually measures, or a spike gets blamed on the frame after the one that caused it.
	var perf: Dictionary = _prev_perf
	_prev_perf = _world.debug_perf_stats()
	for k: String in perf:
		_perf_accum[k] = float(_perf_accum.get(k, 0.0)) + float(perf[k])
		_perf_max[k] = maxf(float(_perf_max.get(k, 0.0)), float(perf[k]))
	# Same pairing for the LoD counters: use the stats from the frame whose delta is being
	# recorded, then refresh the next frame's snapshot.
	var lod: Dictionary = _prev_lod
	_prev_lod = _world.debug_lod_stats()
	_lod_ms_samples.append(float(perf.get("lod_ms", 0.0)))
	_draw_pages_samples.append(float(lod.get("draw_pages", 0)))
	_culled_ratio_samples.append(float(lod.get("culled_ratio", 0.0)))
	_chunks_resident_samples.append(float(lod.get("chunks_resident", 0)))
	_pages_used_samples.append(float(lod.get("pages_used", 0)))
	# Keep the breakdown of the single worst frame: an average cannot say what a spike was,
	# and the spikes are the whole problem — a 9 ms average with a 35 ms frame in it is not
	# 60 fps to anyone holding the mouse.
	if ms > _worst_ms:
		_worst_ms = ms
		_worst = perf.duplicate()

	if _samples.size() >= _target_frames:
		_draining = true

func _terrain_height(x: float, z: float) -> float:
	# Mirror of extension/src/generator/generator.cpp and shaders/field.glslh. Used only to
	# hold the ridge leg close to the floor; the benchmark does not modify the world.
	return 51.2 + (
			6.0 * sin(x * 0.11) * cos(z * 0.13)
			+ 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043)
			+ 1.0 * sin(x * 0.23 + z * 0.19))

func _fire_edit() -> void:
	# Sweep the aim so successive edits hit fresh ground instead of re-carving one hole:
	# that is what a player holding the button down while looking around actually does,
	# and it is the case that dirties the most regions and collision chunks.
	_edit_phase += 0.13
	var dir := Vector3(sin(_edit_phase) * 0.5, -1.0, cos(_edit_phase) * 0.5).normalized()
	var hit: Dictionary = _world.debug_raycast(_cam.global_position, dir)
	if not hit["hit"]:
		return
	_tool.apply_sphere_subtract(hit["pos"], 3.0)

func _island_cycle(delta: float) -> void:
	# Build a pillar, wait for it to stream in, subtract through its middle, then wait for
	# the severed piece to re-merge before building the next. Each cycle puts one
	# connectivity run, one extraction, one spawn and one re-merge inside the sampled window.
	_island_timer += delta
	var base := _player.global_position + Vector3(6.0, -4.0, 6.0)
	if not _island_built:
		for i in range(5):
			_tool.apply_sphere_add(base + Vector3(0, 1.0 * i, 0), 1.2, 4)
		_island_built = true
		_island_waiting_for_merge = false
		_island_timer = 0.0
		return
	if _island_waiting_for_merge:
		# Do not start another pillar until the previous island has fallen asleep and
		# re-merged; otherwise cycles overlap, live islands accumulate, and refusals appear.
		if _island_timer >= 2.0:
			var st: Dictionary = _world.debug_island_stats()
			var live_islands := int(st.get("live_islands", 0))
			if live_islands == 0:
				_island_built = false
				_island_waiting_for_merge = false
				_island_timer = 0.0
		return
	if _island_timer < 1.0:
		return
	_tool.apply_sphere_subtract(base + Vector3(0, 2.0, 0), 1.6)
	_island_waiting_for_merge = true
	_island_timer = 0.0

func _percentile(sorted: PackedFloat32Array, p: float) -> float:
	if sorted.is_empty():
		return 0.0
	var i := int(round(p * (sorted.size() - 1)))
	return sorted[clampi(i, 0, sorted.size() - 1)]

func _append_gpu(key: String, value: float) -> void:
	var values: PackedFloat32Array = _gpu_samples[key]
	values.append(value)
	_gpu_samples[key] = values

func _capture_gpu_sample() -> void:
	if not _world:
		return
	var d: Dictionary = _world.debug_gpu_timings()
	if d.has("timestamp_unit"):
		_gpu_timestamp_unit = str(d["timestamp_unit"])
		_gpu_timestamp_scale = float(d.get("timestamp_scale_to_microseconds", 0.0))
		_gpu_timestamp_normalization = str(d.get("timestamp_normalization", "unavailable"))
		_gpu_timestamp_normalized = bool(d.get("timestamp_normalized", false))
	if not d.get("valid", false):
		return
	var sample_id := int(d["sample_id"])
	if sample_id == _last_gpu_sample_id:
		return
	_last_gpu_sample_id = sample_id
	_gpu_dropped_pairs = max(_gpu_dropped_pairs, int(d.get("dropped_pairs", 0)))
	for key in ["raymarch", "stream", "lod", "ssgi", "ssr", "outlines", "unattributed"]:
		var value := float(d.get(key + "_gpu_ms", -1.0))
		if value >= 0.0:
			_append_gpu(key, value)
	var shadow_ms := 0.0
	var shadow_ran := false
	for key in ["sun_shadow", "contact"]:
		var value := float(d.get(key + "_gpu_ms", -1.0))
		if value >= 0.0:
			shadow_ms += value
			shadow_ran = true
	if shadow_ran:
		_append_gpu("shadows", shadow_ms)
	var custom_frame := float(d.get("custom_frame_gpu_ms", -1.0))
	if custom_frame >= 0.0:
		_append_gpu("custom_frame", custom_frame)

func _budget_verdict(values: PackedFloat32Array, budget_ms: float) -> String:
	if values.size() < MIN_GPU_SAMPLES:
		return "UNMEASURED"
	var sorted := values.duplicate()
	sorted.sort()
	return "PASS" if _percentile(sorted, 0.99) <= budget_ms else "WARN"

func _timing_condition_line() -> String:
	var qualified := _vsync_actual != "disabled"
	return "BENCH timing_condition display_driver=%s vsync_requested=disabled vsync_actual=%s verdict_qualified=%s" % [
		DisplayServer.get_name(), _vsync_actual, str(qualified).to_lower()]

func _report() -> void:
	var sorted := _samples.duplicate()
	sorted.sort()
	var total := 0.0
	var over := 0
	for s in _samples:
		total += s
		if s > TARGET_MS:
			over += 1
	var avg := total / _samples.size()
	print("BENCH mode=%s frames=%d" % [_mode, _samples.size()])
	print("BENCH frame_avg_ms=%.2f fps=%.1f" % [avg, 1000.0 / avg])
	print("BENCH p50=%.2f p95=%.2f p99=%.2f max=%.2f min_fps=%.1f over_16.6ms=%d (%.1f%%)" % [
		_percentile(sorted, 0.50), _percentile(sorted, 0.95), _percentile(sorted, 0.99),
		sorted[sorted.size() - 1], 1000.0 / sorted[sorted.size() - 1],
		over, 100.0 * over / _samples.size()])
	var sorted_lod_ms := _lod_ms_samples.duplicate()
	sorted_lod_ms.sort()
	var sorted_draw_pages := _draw_pages_samples.duplicate()
	sorted_draw_pages.sort()
	var sorted_culled_ratio := _culled_ratio_samples.duplicate()
	sorted_culled_ratio.sort()
	var sorted_chunks_resident := _chunks_resident_samples.duplicate()
	sorted_chunks_resident.sort()
	var sorted_pages_used := _pages_used_samples.duplicate()
	sorted_pages_used.sort()
	print("BENCH lod_summary lod_cpu_record_ms_p50=%.2f lod_cpu_record_ms_p99=%.2f draw_pages_p50=%.0f draw_pages_p99=%.0f culled_ratio_p50=%.3f culled_ratio_p99=%.3f chunks_resident_p50=%.0f chunks_resident_p99=%.0f pages_used_p50=%.0f pages_used_p99=%.0f" % [
		_percentile(sorted_lod_ms, 0.50), _percentile(sorted_lod_ms, 0.99),
		_percentile(sorted_draw_pages, 0.50), _percentile(sorted_draw_pages, 0.99),
		_percentile(sorted_culled_ratio, 0.50), _percentile(sorted_culled_ratio, 0.99),
		_percentile(sorted_chunks_resident, 0.50), _percentile(sorted_chunks_resident, 0.99),
		_percentile(sorted_pages_used, 0.50), _percentile(sorted_pages_used, 0.99)])
	var keys := _perf_accum.keys()
	keys.sort()
	var parts := PackedStringArray()
	for k: String in keys:
		parts.append("%s=%.2f" % [k, float(_perf_accum[k]) / _samples.size()])
	print("BENCH avg_ms " + " ".join(parts))
	var maxparts := PackedStringArray()
	for k: String in keys:
		maxparts.append("%s=%.2f" % [k, float(_perf_max[k])])
	print("BENCH max_ms " + " ".join(maxparts))
	var worstparts := PackedStringArray()
	for k: String in keys:
		worstparts.append("%s=%.2f" % [k, float(_worst.get(k, 0.0))])
	print("BENCH worst_frame(%.2fms) " % _worst_ms + " ".join(worstparts))
	for key in ["raymarch", "stream", "lod", "ssgi", "ssr", "shadows", "outlines",
			"unattributed", "custom_frame"]:
		var values: PackedFloat32Array = _gpu_samples[key]
		var sorted_gpu := values.duplicate()
		sorted_gpu.sort()
		print("BENCH gpu_%s samples=%d p50_ms=%.3f p99_ms=%.3f" % [key, values.size(),
			_percentile(sorted_gpu, 0.50), _percentile(sorted_gpu, 0.99)])
	var frame_sorted := _samples.duplicate()
	frame_sorted.sort()
	var verdict := {
		"raymarch": _budget_verdict(_gpu_samples["raymarch"], BUDGETS_MS["raymarch"]),
		"lod": _budget_verdict(_gpu_samples["lod"], BUDGETS_MS["lod"]),
		"ssgi": _budget_verdict(_gpu_samples["ssgi"], BUDGETS_MS["ssgi"]),
		"ssr": _budget_verdict(_gpu_samples["ssr"], BUDGETS_MS["ssr"]),
		"shadows": _budget_verdict(_gpu_samples["shadows"], BUDGETS_MS["shadows"]),
		"outlines": _budget_verdict(_gpu_samples["outlines"], BUDGETS_MS["outlines"]),
		"frame": "PASS" if _percentile(frame_sorted, 0.99) <= BUDGETS_MS["frame"] else "WARN",
	}
	print("BENCH budget_verdict raymarch=%s lod=%s ssgi=%s ssr=%s shadows=%s outlines=%s frame=%s" % [
		verdict["raymarch"], verdict["lod"], verdict["ssgi"], verdict["ssr"],
		verdict["shadows"], verdict["outlines"], verdict["frame"]])
	var custom_values: PackedFloat32Array = _gpu_samples["custom_frame"]
	print("BENCH gpu_timing valid_samples=%d dropped_pairs=%d lod_source=timestamp lod_ms_source=cpu_record" % [
		custom_values.size(), _gpu_dropped_pairs])
	print("BENCH gpu_timestamp_normalization mode=%s unit=%s scale_to_us=%.6f normalized=%s" % [
		_gpu_timestamp_normalization, _gpu_timestamp_unit, _gpu_timestamp_scale,
		str(_gpu_timestamp_normalized).to_lower()])
	var qualified := _vsync_actual != "disabled"
	print(_timing_condition_line())
	if qualified:
		push_warning("BENCH: V-Sync is %s; frame percentiles are display-capped, not engine numbers" % _vsync_actual)
	var st: Dictionary = _world.debug_stream_stats()
	print("BENCH regions=%d overflow=%d overrides=%d/%d consolidations=%d refusals=%d" % [
		st.get("resident_regions", -1), st.get("overflow_ever", -1),
		st.get("override_bricks", -1), st.get("override_capacity", -1),
		st.get("consolidations", -1), st.get("consolidation_refusals", -1)])
	var ph: Dictionary = _world.debug_physics_stats()
	print("BENCH chunks=%d pending=%d bodies=%d bodies_raw=%d failures=%d build_ms=%.2f collect_ms=%.2f" % [
		ph.get("chunks_resident", -1), ph.get("chunks_pending", -1), ph.get("bodies", -1),
		ph.get("bodies_raw", -1), ph.get("failures", -1), ph.get("build_ms", 0.0),
		ph.get("collect_ms", 0.0)])
	var isl: Dictionary = _world.debug_island_stats()
	print("BENCH islands=%d debris=%d spawned=%d merged=%d refused=%d cx_runs=%d" % [
		isl.get("live_islands", -1), isl.get("live_debris", -1),
		isl.get("islands_spawned", -1), isl.get("islands_merged", -1),
		isl.get("refused", -1), isl.get("connectivity_runs", -1)])
	if avg > TARGET_MS:
		push_warning("BENCH: frame budget exceeded (target %.1fms)" % TARGET_MS)
