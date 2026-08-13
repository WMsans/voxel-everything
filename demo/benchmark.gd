extends Node
# Runs only when `--benchmark` is passed after `--` on the command line.

const FRAMES := 300

var _frames := 0
var _accum_ms := 0.0
var _active := false

func _ready() -> void:
	if "--benchmark" in OS.get_cmdline_user_args():
		_active = true
		var cam: Camera3D = get_parent().get_node("Camera3D")
		cam.transform = Transform3D(Basis.looking_at(Vector3(6, -10, 6).normalized()), Vector3(24, 12, 24))
		cam.set_script(null) # freeze: no fly-camera movement

func _process(_delta: float) -> void:
	if not _active:
		return
	_frames += 1
	_accum_ms += 1000.0 / maxf(float(Engine.get_frames_per_second()), 0.001)
	if _frames >= FRAMES:
		var avg := _accum_ms / FRAMES
		print("BENCH frame_avg_ms=%.2f fps=%.1f" % [avg, 1000.0 / avg])
		if avg > 16.6:
			push_warning("BENCH: frame budget exceeded (target 16.6ms)")
		get_tree().quit()
