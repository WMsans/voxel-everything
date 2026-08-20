extends GdUnitTestSuite

const CAPTURE_SCRIPT := preload("res://demo/capture.gd")
var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

func make_capture() -> Node:
	var n := Node.new()
	n.set_script(CAPTURE_SCRIPT)
	add_child(n)
	_nodes.append(n)
	return n

# Determinism is the whole feature: the same frame index gives the same camera, every run,
# on every machine, at any frame rate.
func test_camera_is_a_pure_function_of_the_frame() -> void:
	var c := make_capture()
	for f in [0, 137, 448, 899]:
		var a: Transform3D = c.camera_at(f)
		var b: Transform3D = c.camera_at(f)
		assert_vector(a.origin).is_equal(b.origin)
	assert_vector(c.camera_at(0).origin).is_not_equal(c.camera_at(300).origin)

# The path has to stay inside the world and above the ground, or the capture is 900 frames
# of the inside of a hill.
func test_path_stays_in_bounds_and_above_the_terrain() -> void:
	var c := make_capture()
	for f in range(0, 900, 25):
		var p: Vector3 = c.camera_at(f).origin
		assert_float(p.x).is_between(8.0, 4088.0)
		assert_float(p.z).is_between(8.0, 4088.0)
		assert_float(p.y).is_greater(c.terrain_height(p.x, p.z) + 1.0)

# The destruction script fires at known frames, so a recut of the video can find the beats.
func test_events_are_scheduled_and_bounded() -> void:
	var c := make_capture()
	var total := 0
	for f in range(900):
		total += c.events_at(f).size()
	assert_int(total).is_greater(4)
	assert_int(total).is_less(40)
