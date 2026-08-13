extends Label

func _process(_delta: float) -> void:
	var fps := Engine.get_frames_per_second()
	var ms := 1000.0 / maxf(float(fps), 0.001)
	text = "%d fps  (%.1f ms)" % [fps, ms]
