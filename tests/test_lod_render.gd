extends GdUnitTestSuite

# The far field on screen (spec section 7). Every test here looks at the same converged
# world from the same camera, so the world is built ONCE for the whole suite: settling it
# is the only expensive thing in the file, and re-running it per test bought nothing.

var _world: VoxelWorld
var _settled := false

const POS := Vector3(100.0, 155.0, 204.0)
const FWD := Vector3(0.0, -0.35, -1.0)

# The walk only descends into a node whose eight children are all resident, so the far field
# converges over hundreds of ticks, not a fixed handful. Waiting on the CONDITION instead of
# a frame count keeps the test honest whatever the machine's build throughput is; this
# camera needs ~350 ticks, so the budget is a wide margin, not a target.
const SETTLE_BUDGET := 2500
# requests_pending is read from the walk that ran BEFORE this tick's results were collected,
# so it dips to zero for a tick or two while a batch is landing. Convergence has to hold for
# a streak or the settle stops before anything is built.
const QUIET_TICKS := 8

func after() -> void:
	if is_instance_valid(_world):
		_world.free()
	_world = null

func settled_world() -> VoxelWorld:
	if _settled:
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

	var fwd := FWD.normalized()
	var quiet := 0
	var ticks := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(POS, fwd)
		await get_tree().process_frame
		var d := w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		ticks = i + 1
		if quiet >= QUIET_TICKS:
			break
	assert_int(quiet).override_failure_message(
		"the far field never converged in %d ticks: %s" % [ticks, w.debug_lod_stats()]
		).is_greater_equal(QUIET_TICKS)
	_settled = true
	return w

func test_the_far_field_covers_the_ground(timeout := 60000) -> void:
	var w: VoxelWorld = await settled_world()
	var r := w.debug_lod_render_probe(POS, FWD.normalized(), 256, 200)
	assert_int(r["draw_pages"]).override_failure_message(
		"nothing was submitted to the draw: %s" % r).is_greater(0)
	# Looking down at terrain from 90 m: the lower half of the frame must be covered.
	assert_float(r["coverage"]).override_failure_message(
		"the far field covered %.3f of the frame" % r["coverage"]).is_greater(0.25)

func test_depth_is_written_so_the_near_field_can_occlude(timeout := 60000) -> void:
	var w: VoxelWorld = await settled_world()
	var r := w.debug_lod_render_probe(POS, FWD.normalized(), 256, 200)
	# Reverse-Z: every covered pixel must hold a depth strictly between far (0) and near (1).
	assert_float(r["depth_min"]).is_greater(0.0)
	assert_float(r["depth_max"]).is_less(1.0)
	assert_float(r["depth_max"]).is_greater(r["depth_min"])

func test_nothing_is_drawn_inside_the_near_field(timeout := 60000) -> void:
	var w: VoxelWorld = await settled_world()
	var r := w.debug_lod_render_probe(POS, FWD.normalized(), 256, 200)
	# Spec section 6.4: a chunk entirely inside the fade start is never even built.
	assert_float(r["nearest_hit_m"]).override_failure_message(
		"the far field drew geometry at %.1f m, inside the 120 m fade start" % r["nearest_hit_m"]
		).is_greater_equal(100.0)

func test_backface_culling_does_not_remove_visible_ground(timeout := 60000) -> void:
	var w: VoxelWorld = await settled_world()
	var fwd := FWD.normalized()
	# Each probe ticks the world before it draws, so the two are only comparable once the
	# far field has converged and a tick no longer changes what is resident -- which is
	# exactly the state settled_world() hands back.
	var off := w.debug_lod_render_probe_culled(POS, fwd, 256, 200, false)
	var on := w.debug_lod_render_probe_culled(POS, fwd, 256, 200, true)
	# M3 errata 1: this codebase's winding convention has already cost one bug, so the
	# front-face setting is MEASURED, not assumed. Culling backfaces must not lose coverage.
	assert_float(on["coverage"]).override_failure_message(
		"backface culling dropped coverage from %.3f to %.3f: the front-face setting is wrong"
		% [off["coverage"], on["coverage"]]).is_greater(off["coverage"] * 0.95)
