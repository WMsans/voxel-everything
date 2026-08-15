extends Node
# Frame-time harness. Runs only when a --benchmark* flag is passed after `--`.
#
#   --benchmark        steady state: the player is frozen, nothing streams after warmup.
#   --benchmark-move   the player flies forward continuously, so regions and collision
#                      chunks stream in for the whole run.
#   --benchmark-edit   the player is frozen and the edit tool fires every frame.
#
# The last two are the cases players actually complain about, and an AVERAGE hides them:
# a run that is 8 ms most of the time and 50 ms whenever a batch lands reads as "fine".
# So the report is percentiles plus the count of frames that missed 60 fps.

const WARMUP := 60
const FRAMES := 300
const TARGET_MS := 16.6

var _mode := ""
var _frames := 0
var _samples: PackedFloat32Array = PackedFloat32Array()
var _perf_accum := {}
var _perf_max := {}
var _prev_perf := {}
var _worst := {}
var _worst_ms := 0.0
var _player: CharacterBody3D
var _world: VoxelWorld
var _tool: VoxelEditTool
var _cam: Camera3D
var _edit_phase := 0.0

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	for m in ["--benchmark-move", "--benchmark-edit", "--benchmark"]:
		if m in args:
			_mode = m
			break
	if _mode == "":
		return

	_player = get_parent().get_node("Player")
	_world = get_parent().get_node("VoxelWorld")
	_cam = _player.get_node("Camera3D")
	# Drive the player from here rather than from input, so a run is reproducible.
	_player.set_physics_process(false)
	_player.set_process_unhandled_input(false)
	_player.global_transform = Transform3D(Basis.IDENTITY, Vector3(24, 63.2, 24))
	_cam.transform = Transform3D(Basis.looking_at(Vector3(6, -10, 6).normalized()),
		Vector3(0, 0.7, 0))
	if _mode == "--benchmark-edit":
		_tool = ClassDB.instantiate("VoxelEditTool")
		_world.add_child(_tool)

func _process(delta: float) -> void:
	if _mode == "":
		return
	_frames += 1

	if _mode == "--benchmark-move":
		# Straight line across the world at fly speed: a steady stream of new regions and
		# collision chunks, which is the "walking forward" case.
		_player.global_position += Vector3(0.7, 0.0, 0.7).normalized() * 25.0 * delta
	elif _mode == "--benchmark-edit" and _frames > WARMUP:
		_fire_edit()

	if _frames <= WARMUP:
		return # let the first regions land before sampling
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
	# Keep the breakdown of the single worst frame: an average cannot say what a spike was,
	# and the spikes are the whole problem — a 9 ms average with a 35 ms frame in it is not
	# 60 fps to anyone holding the mouse.
	if ms > _worst_ms:
		_worst_ms = ms
		_worst = perf.duplicate()

	if _samples.size() >= FRAMES:
		_report()
		get_tree().quit()

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

func _percentile(sorted: PackedFloat32Array, p: float) -> float:
	if sorted.is_empty():
		return 0.0
	var i := int(round(p * (sorted.size() - 1)))
	return sorted[clampi(i, 0, sorted.size() - 1)]

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
	var st: Dictionary = _world.debug_stream_stats()
	print("BENCH regions=%d overflow=%d" % [st.get("resident_regions", -1),
		st.get("overflow_ever", -1)])
	var ph: Dictionary = _world.debug_physics_stats()
	print("BENCH chunks=%d pending=%d bodies=%d failures=%d build_ms=%.2f collect_ms=%.2f" % [
		ph.get("chunks_resident", -1), ph.get("chunks_pending", -1), ph.get("bodies", -1),
		ph.get("failures", -1), ph.get("build_ms", 0.0), ph.get("collect_ms", 0.0)])
	if avg > TARGET_MS:
		push_warning("BENCH: frame budget exceeded (target %.1fms)" % TARGET_MS)
