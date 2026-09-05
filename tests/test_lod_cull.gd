extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0 # small but viable: the walk descends only into a node
	# whose eight children are all in-radius (L6 spans 819 m); below ~940 m it stalls
	# at 2 roots, so 1000 is the floor for these cameras
	w.max_lod_pages = 8192
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# The walk descends only into a node whose eight children are all resident, so the far field
# converges over HUNDREDS of ticks and at a rate set by build throughput, not by frame count.
# Wait on the condition (errata 6): fixed frame counts were tuned to the old occluded
# 590 ms vsync frame and settle nothing on the current runner, which keeps vsync enabled
# intentionally (gdunit_tests.sh) at a normal display rate.
# requests_pending comes from the walk that ran BEFORE this tick collected its results, so it
# dips to zero for a tick or two while a batch lands -- the streak is what makes it mean
# "converged" rather than "between batches". Measured: ~350-400 ticks, so the budget is margin.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# The cull only ever ZEROES instanceCount. It must never change a page's vertexOffset or its
# index count, because those are what address the arena -- a cull that rewrote them would
# draw one chunk's geometry at another chunk's origin.
func test_the_cull_only_removes(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.hooks().debug_lod_cull_probe(pos, fwd)
	assert_int(d["args_before"]).is_greater(0)
	assert_int(d["args_after"]).is_equal(d["args_before"])
	assert_int(d["offsets_changed"]).override_failure_message(
		"the cull rewrote %d vertex offsets" % d["offsets_changed"]).is_equal(0)
	assert_int(d["index_counts_changed"]).is_equal(0)
	assert_int(d["drawn_after"]).is_less_equal(d["args_before"])

func test_facing_away_culls_almost_everything(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var facing := w.hooks().debug_lod_cull_probe(pos, fwd)
	# Same resident set, camera spun to face straight up into empty sky.
	var away := w.hooks().debug_lod_cull_probe(pos, Vector3(0.0, 1.0, 0.0))
	assert_int(away["drawn_after"]).override_failure_message(
		"looking at the sky still drew %d pages" % away["drawn_after"]
		).is_less(facing["drawn_after"] / 2)

func test_the_reported_ratio_is_sane(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.hooks().debug_lod_cull_probe(pos, fwd)
	assert_float(d["culled_ratio"]).is_between(0.0, 1.0)

# The args list is compact (walk order) while the arena page ids are arbitrary. The cull
# must recover each slot's real page id from args[slot*5+3] / 2048 and cull using that page's
# chunk AABB. This test finds slots whose real page AABB is inside the frustum while the
# slot-index chunk AABB is outside; the old shader culled all of them because it used the
# slot as the page id, while the fixed shader keeps them (they are not frustum-culled).
func test_cull_uses_real_arena_page_id(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.hooks().debug_lod_cull_probe(pos, fwd)
	var page_frustum: PackedInt32Array = d["page_frustum_culled"]
	var slot_frustum: PackedInt32Array = d["slot_frustum_culled"]
	var culled: PackedInt32Array = d["culled"]
	assert_int(page_frustum.size()).is_greater(0)
	var wrong_slot_outside := 0
	var kept_despite_wrong_slot := 0
	for i in range(page_frustum.size()):
		if page_frustum[i] == 0 and slot_frustum[i] == 1:
			wrong_slot_outside += 1
			if culled[i] == 0:
				kept_despite_wrong_slot += 1
	assert_int(wrong_slot_outside).override_failure_message(
		"no draw slot had a real page inside the frustum while its slot-index chunk was outside; cannot exercise slot-vs-page conflation"
		).is_greater(0)
	assert_int(kept_despite_wrong_slot).override_failure_message(
		"cull culled %d pages whose real arena page AABB was in the frustum but whose slot-index AABB was outside; the compact slot was treated as the arena page id" % kept_despite_wrong_slot
		).is_greater(0)
