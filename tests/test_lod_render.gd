extends GdUnitTestSuite

var _world: VoxelWorld

func after() -> void:
	if is_instance_valid(_world):
		_world.free()
	_world = null

func make_world() -> VoxelWorld:
	if _world != null:
		return _world
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.max_lod_pages = 4096
	add_child(w)
	_world = w
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3, frames: int, yield_every := 2) -> void:
	for i in range(frames):
		w.debug_lod_tick(pos, fwd)
		if i % yield_every == yield_every - 1:
			await get_tree().process_frame

func test_the_far_field_covers_the_ground(timeout := 120000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 155.0, 204.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd, 80)
	var r := w.debug_lod_render_probe(pos, fwd, 256, 200)
	assert_int(r["draw_pages"]).override_failure_message(
		"nothing was submitted to the draw: %s" % r).is_greater(0)
	# Looking down at terrain from 90 m: the lower half of the frame must be covered.
	assert_float(r["coverage"]).override_failure_message(
		"the far field covered %.3f of the frame" % r["coverage"]).is_greater(0.25)

func test_depth_is_written_so_the_near_field_can_occlude(timeout := 120000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 155.0, 204.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd, 80)
	var r := w.debug_lod_render_probe(pos, fwd, 256, 200)
	# Reverse-Z: every covered pixel must hold a depth strictly between far (0) and near (1).
	assert_float(r["depth_min"]).is_greater(0.0)
	assert_float(r["depth_max"]).is_less(1.0)
	assert_float(r["depth_max"]).is_greater(r["depth_min"])

func test_nothing_is_drawn_inside_the_near_field(timeout := 120000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 155.0, 204.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd, 80)
	var r := w.debug_lod_render_probe(pos, fwd, 256, 200)
	# Spec section 6.4: a chunk entirely inside the fade start is never even built.
	assert_float(r["nearest_hit_m"]).override_failure_message(
		"the far field drew geometry at %.1f m, inside the 120 m fade start" % r["nearest_hit_m"]
		).is_greater_equal(100.0)

func test_backface_culling_does_not_remove_visible_ground(timeout := 120000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 155.0, 204.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd, 80)
	var off := w.debug_lod_render_probe_culled(pos, fwd, 256, 200, false)
	var on := w.debug_lod_render_probe_culled(pos, fwd, 256, 200, true)
	# M3 errata 1: this codebase's winding convention has already cost one bug, so the
	# front-face setting is MEASURED, not assumed. Culling backfaces must not lose coverage.
	assert_float(on["coverage"]).override_failure_message(
		"backface culling dropped coverage from %.3f to %.3f: the front-face setting is wrong"
		% [off["coverage"], on["coverage"]]).is_greater(off["coverage"] * 0.95)
