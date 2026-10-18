extends GdUnitTestSuite

# S8 (docs/superpowers/specs/2026-09-13-frame-module-design.md §10): the "deferred" GPU timing
# scope contained the "ssao" scope, so "deferred" reported SSAO's time as its own and
# "unattributed" subtracted SSAO twice. Timestamp VALUES are invalid on this machine, but the
# capture's marker NAMES arrive in command order (verified on Metal, 2026-09-14), so this pins
# the order: ssao's end marker precedes deferred's begin marker within one frame serial.
# Since 2026-10-18 AO comes out of the "ssgi" horizon pass, so that is the scope pinned.

const W := 128
const H := 72

var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

# Index of marker ve:<serial>:<pass>:<any occurrence>:<side>, or -1.
func marker_index(names: PackedStringArray, serial: String, pass_name: String, side: String) -> int:
	for i in range(names.size()):
		var f := names[i].split(":")
		if f.size() == 5 and f[0] == "ve" and f[1] == serial and f[2] == pass_name and f[4] == side:
			return i
	return -1

func test_horizon_scope_closes_before_deferred_opens(timeout := 120000) -> void:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = false
	world.physics_enabled = false
	add_child(world)
	_nodes.append(world)
	world.set_effect_enabled("ssao", true)
	var raymarch: RaymarchCompositor = ClassDB.instantiate("RaymarchCompositor")
	raymarch.world_path = world.get_path()
	var beauty: BeautyCompositor = ClassDB.instantiate("BeautyCompositor")
	beauty.world_path = world.get_path()
	var effects: Array[CompositorEffect] = [raymarch, beauty]
	var compositor := Compositor.new()
	compositor.compositor_effects = effects
	var vp := SubViewport.new()
	vp.size = Vector2i(W, H)
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(vp)
	_nodes.append(vp)
	var cam := Camera3D.new()
	cam.fov = 60.0
	cam.near = 0.05
	cam.far = 4000.0
	cam.compositor = compositor
	vp.add_child(cam)
	cam.current = true
	cam.global_position = Vector3(30.0, 70.0, 30.0)
	cam.look_at(Vector3(30.2, 69.0, 30.2), Vector3.UP)

	var rd := RenderingServer.get_rendering_device()
	var horizon_end := -1
	var deferred_begin := -1
	var names := PackedStringArray()
	for frame in range(600):
		await RenderingServer.frame_post_draw
		names = PackedStringArray()
		for k in range(rd.get_captured_timestamps_count()):
			names.append(rd.get_captured_timestamp_name(k))
		var serial := ""
		for n in names:
			var f := n.split(":")
			if f.size() == 5 and f[0] == "ve" and f[2] == "deferred" and f[4] == "b":
				serial = f[1]
		if serial.is_empty():
			continue
		horizon_end = marker_index(names, serial, "ssgi", "e")
		deferred_begin = marker_index(names, serial, "deferred", "b")
		if horizon_end >= 0 and deferred_begin >= 0:
			break
	assert_int(deferred_begin).override_failure_message(
		"no captured frame held both scopes: %s" % names).is_greater(-1)
	assert_int(horizon_end).override_failure_message(
		"no captured frame held both scopes: %s" % names).is_greater(-1)
	assert_int(horizon_end).override_failure_message(
		"ssgi closes at %d, after deferred opened at %d: %s" % [horizon_end, deferred_begin, names]
		).is_less(deferred_begin)
