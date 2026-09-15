extends GdUnitTestSuite

# Render lifetime contract (docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md
# §5.1). Characterization written BEFORE sub-project 2 moves any lifetime code: it pins what
# teardown, reload and shutdown do today, so "the move changed nothing" is a measurement.
#
# Leaks are measured only on the LOCAL device: freeing it makes the engine name every RID that
# is still alive ("N RID(s) of type "X" was leaked"). On this machine the RenderingDevice
# allocation counters read 0 and memory usage never decreases, and the main device is never
# freed, so main-device leaks are not measurable here and are not asserted.

const Golden := preload("res://tests/test_frame_shipped_golden.gd")

const CAM := Vector3(24.0, 70.0, 24.0)
const FWD := Vector3(0.2, -1.0, 0.2)
const TEARDOWN_ORDER := ["passes", "streamer", "residency", "island_graph", "island_slots",
	"atlas", "lod", "history", "initialized"]

class LeakLogger extends Logger:
	var lines := PackedStringArray()

	func _log_error(_function: String, _file: String, _line: int, code: String, rationale: String,
			_editor_notify: bool, _error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
		var text := "%s %s" % [code, rationale]
		if text.contains(" leaked"):
			lines.append(text)

	func _log_message(message: String, _error: bool) -> void:
		if message.contains(" leaked"):
			lines.append(message)

var _nodes: Array = []
var _logger: LeakLogger

func before_test() -> void:
	_logger = LeakLogger.new()
	OS.add_logger(_logger)

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()
	OS.remove_logger(_logger)

func make_local_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_nodes.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	settle(w)
	return w

# Several consecutive quiet streamer frames (see test_shader_reload.gd for why one is not enough).
func settle(w: VoxelWorld) -> void:
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(CAM) == 0 else 0
		if quiet >= 6:
			return

func render(w: VoxelWorld) -> Dictionary:
	return w.hooks().debug_render_frame(CAM, FWD.normalized(), 64, 64)

func assert_renders(w: VoxelWorld, why: String) -> void:
	var d := render(w)
	assert_bool(d["ok"]).override_failure_message("%s: frame aborted: %s" % [why, d]).is_true()
	assert_float(float(d["mean_luma"])).override_failure_message(
		"%s: the lit image is black" % why).is_greater(0.01)

# Frees the world -- _exit_tree shuts render resources down and drops the local device -- and
# returns the leak lines the engine printed while doing so.
func free_and_collect_leaks(w: VoxelWorld) -> PackedStringArray:
	_nodes.erase(w)
	_logger.lines.clear()
	w.free()
	return _logger.lines.duplicate()

func test_teardown_then_reinit_renders_and_frees_clean(timeout := 120000) -> void:
	var w := make_local_world()
	assert_renders(w, "before teardown")
	w.hooks().debug_teardown_atlas()
	assert_bool(w.is_initialized()).is_false()
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	settle(w)
	assert_renders(w, "after teardown + re-init")
	var leaks := free_and_collect_leaks(w)
	assert_array(Array(leaks)).override_failure_message(
		"teardown + re-init left RIDs alive: %s" % leaks).is_empty()

func test_reload_pump_renders_and_frees_clean(timeout := 120000) -> void:
	var w := make_local_world()
	assert_renders(w, "before reload")
	w.request_shader_reload()
	w.hooks().debug_pump_shader_reload()
	assert_bool(w.hooks().debug_shader_reload_stats()["last_ok"]).is_true()
	settle(w)
	assert_renders(w, "after reload")
	var leaks := free_and_collect_leaks(w)
	assert_array(Array(leaks)).override_failure_message(
		"reload left RIDs alive: %s" % leaks).is_empty()

func test_shutdown_closes_admission_and_frees_clean(timeout := 60000) -> void:
	var w := make_local_world()
	assert_renders(w, "before shutdown")
	w.shutdown_render_resources()
	assert_bool(w.is_initialized()).is_false()
	assert_bool(w.try_begin_render_callback()).override_failure_message(
		"admission reopened after shutdown").is_false()
	var leaks := free_and_collect_leaks(w)
	assert_array(Array(leaks)).override_failure_message(
		"shutdown left RIDs alive: %s" % leaks).is_empty()

# An upload queued before a GPU teardown is part of the CPU world and must reach the next pool.
# (Physics teardown's filter is pinned by test_island_render.gd.)
func test_a_queued_upload_survives_gpu_teardown(timeout := 120000) -> void:
	var w := make_local_world()
	assert_renders(w, "drain anything already queued")
	var before: int = w.hooks().debug_field_volume_upload_count()
	var dim := 2
	var bytes := PackedByteArray()
	bytes.resize(dim * dim * dim)
	bytes.fill(128)
	w.hooks().debug_queue_committed_field_volume_upload(5, bytes, bytes, dim)
	assert_int(w.hooks().debug_island_pending_uploads()).is_equal(1)
	w.hooks().debug_teardown_atlas()
	assert_int(w.hooks().debug_island_pending_uploads()).override_failure_message(
		"GPU teardown dropped a queued upload").is_equal(1)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	settle(w)
	assert_renders(w, "after re-init")
	assert_int(w.hooks().debug_island_pending_uploads()).is_equal(0)
	assert_int(w.hooks().debug_field_volume_upload_count()).override_failure_message(
		"the upload queued before teardown never landed").is_equal(before + 1)

func test_gpu_teardown_order_is_pinned(timeout := 60000) -> void:
	var w := make_local_world()
	w.hooks().debug_teardown_atlas()
	var trace: PackedStringArray = w.hooks().debug_teardown_trace()
	assert_array(Array(trace)).is_equal(TEARDOWN_ORDER)

# --- the shipped path (main device, real compositors). Mirrors test_frame_shipped_golden.gd's
# fixture; camera, sizes and tolerance come from that suite so the two cannot drift. ---

func make_shipped_scene() -> Dictionary:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = false
	world.physics_enabled = false
	add_child(world)
	_nodes.append(world)
	assert_bool(world.hooks().debug_init_physics()).is_true()
	var raymarch: RaymarchCompositor = ClassDB.instantiate("RaymarchCompositor")
	raymarch.world_path = world.get_path()
	var beauty: BeautyCompositor = ClassDB.instantiate("BeautyCompositor")
	beauty.world_path = world.get_path()
	var effects: Array[CompositorEffect] = [raymarch, beauty]
	var compositor := Compositor.new()
	compositor.compositor_effects = effects
	var vp := SubViewport.new()
	vp.size = Vector2i(Golden.W, Golden.H)
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
	var pose: Array = Golden.CAMERAS["down_close"]
	var pos: Vector3 = pose[0]
	cam.global_position = pos
	cam.look_at(pos + (pose[1] as Vector3).normalized(), Vector3.UP)
	return {"world": world, "viewport": vp}

func settle_shipped(world: VoxelWorld) -> bool:
	for i in range(Golden.SETTLE_FRAMES):
		await get_tree().process_frame
	var quiet := 0
	for i in range(Golden.LOD_SETTLE_BUDGET):
		await get_tree().process_frame
		var s: Dictionary = world.hooks().debug_lod_stats()
		var idle: bool = int(s.get("requests_pending", 1)) == 0 and int(s.get("builds_in_flight", 1)) == 0
		quiet = quiet + 1 if idle else 0
		if quiet >= Golden.LOD_QUIET_FRAMES:
			return true
	return false

func tile_luma(img: Image) -> PackedFloat32Array:
	var sums := PackedFloat32Array()
	var counts := PackedInt32Array()
	sums.resize(Golden.TILES * Golden.TILES)
	counts.resize(Golden.TILES * Golden.TILES)
	for y in range(0, img.get_height(), Golden.SAMPLE_STEP):
		var ty := mini(y * Golden.TILES / img.get_height(), Golden.TILES - 1)
		for x in range(0, img.get_width(), Golden.SAMPLE_STEP):
			var tx := mini(x * Golden.TILES / img.get_width(), Golden.TILES - 1)
			var c := img.get_pixel(x, y)
			sums[ty * Golden.TILES + tx] += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			counts[ty * Golden.TILES + tx] += 1
	for i in range(sums.size()):
		sums[i] = sums[i] / maxf(1.0, float(counts[i]))
	return sums

func test_the_shipped_path_survives_reload_and_shuts_down(timeout := 900000) -> void:
	var s := make_shipped_scene()
	var world: VoxelWorld = s["world"]
	var vp: SubViewport = s["viewport"]
	assert_bool(await settle_shipped(world)).override_failure_message(
		"the far field never settled before reload").is_true()
	world.request_shader_reload()
	for i in range(8):
		await get_tree().process_frame
	var stats: Dictionary = world.hooks().debug_shader_reload_stats()
	assert_int(int(stats["reloads"])).is_equal(1)
	assert_bool(stats["last_ok"]).is_true()
	assert_bool(await settle_shipped(world)).override_failure_message(
		"the far field never settled after reload").is_true()
	var acc := PackedFloat32Array()
	acc.resize(Golden.TILES * Golden.TILES)
	for f in range(Golden.AVERAGE_FRAMES):
		await RenderingServer.frame_post_draw
		var t := tile_luma(vp.get_texture().get_image())
		for i in range(t.size()):
			acc[i] += t[i] / float(Golden.AVERAGE_FRAMES)
	var golden: Array = Golden.GOLDEN["down_close"]["tiles"]
	for i in range(acc.size()):
		assert_float(acc[i]).override_failure_message(
			"tile %d after reload: %f vs golden %f" % [i, acc[i], golden[i]]
			).is_equal_approx(float(golden[i]), Golden.TOL_TILE)
	world.shutdown_render_resources()
	assert_bool(world.is_initialized()).is_false()
	assert_bool(world.try_begin_render_callback()).is_false()
