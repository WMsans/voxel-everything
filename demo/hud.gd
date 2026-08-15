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
		s = "regions %d  edits %d  ovf %d" % [
			st.get("resident_regions", 0), st.get("frame_edits", 0),
			st.get("overflow_ever", 0)]
	text = "%d fps  (%.1f ms)  |  %s" % [fps, ms, s]
