extends GdUnitTestSuite
# Characterization for sub-project 5b (docs/superpowers/plans/2026-09-17-edit-pipeline.md,
# Task 2): which consumer hears which edit, and what each one will act on. Every later task
# must leave these unchanged; Task 10 (staleness) is the only task allowed to move a pin, and
# it moves one in test_connectivity.gd, not here.
#
# Goldens are recorded, not derived: run once, paste each printed "<NAME> <json>" line's JSON
# into the matching const.
#
# debug_edit_fanout() drains every deferred consumer through its own drain first, so the rows
# say what each consumer WILL act on, whether it acts now (today) or at its next tick (once
# the sinks are deferred). Collider rows are expanded chunk SETS and the ops below are tens of
# metres apart, so merging overlapping queue entries cannot move a row. The consolidation
# cases use overlapping ops that share their y and z extents, where a merged bounding box is
# exactly the union.

const EDITS_GOLDEN := {"add":{"colliders":[[1,5,1],[1,5,2],[1,6,1],[1,6,2],[2,5,1],[2,5,2],[2,6,1],[2,6,2],[9,5,1],[9,5,2],[9,6,1],[9,6,2],[10,5,1],[10,5,2],[10,6,1],[10,6,2]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5],[[78,46,14],[81,49,17],2,0.0]],"pending_edits":2,"pending_regions":[[0,1,0],[2,1,0]]},"border":{"colliders":[[1,5,1],[1,5,2],[1,6,1],[1,6,2],[2,5,1],[2,5,2],[2,6,1],[2,6,2],[3,5,9],[3,5,10],[3,6,9],[3,6,10],[4,5,9],[4,5,10],[4,6,9],[4,6,10],[9,5,1],[9,5,2],[9,6,1],[9,6,2],[10,5,1],[10,5,2],[10,6,1],[10,6,2],[17,5,1],[17,5,2],[17,6,1],[17,6,2],[18,5,1],[18,5,2],[18,6,1],[18,6,2],[25,5,1],[25,5,2],[25,6,1],[25,6,2],[26,5,1],[26,5,2],[26,6,1],[26,6,2]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5],[[78,46,14],[81,49,17],2,0.0],[[207,48,16],[209,49,17],4,0.0],[[30,46,78],[33,49,81],5,1.0]],"pending_edits":5,"pending_regions":[[0,1,0],[0,1,2],[1,1,2],[2,1,0],[4,1,0],[6,1,0]]},"fill":{"colliders":[[1,5,1],[1,5,2],[1,5,17],[1,5,18],[1,6,1],[1,6,2],[1,6,17],[1,6,18],[2,5,1],[2,5,2],[2,5,17],[2,5,18],[2,6,1],[2,6,2],[2,6,17],[2,6,18],[3,5,9],[3,5,10],[3,6,9],[3,6,10],[4,5,9],[4,5,10],[4,6,9],[4,6,10],[9,5,1],[9,5,2],[9,6,1],[9,6,2],[10,5,1],[10,5,2],[10,6,1],[10,6,2],[17,5,1],[17,5,2],[17,6,1],[17,6,2],[18,5,1],[18,5,2],[18,6,1],[18,6,2],[25,5,1],[25,5,2],[25,6,1],[25,6,2],[26,5,1],[26,5,2],[26,6,1],[26,6,2]],"consolidation_queue":[[0,1,4]],"edit_rejections":0,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5],[[78,46,14],[81,49,17],2,0.0],[[207,48,16],[209,49,17],4,0.0],[[30,46,78],[33,49,81],5,1.0]],"pending_edits":261,"pending_regions":[[0,1,0],[0,1,2],[0,1,4],[1,1,2],[2,1,0],[4,1,0],[6,1,0]]},"oversized":{"colliders":[[1,5,1],[1,5,2],[1,5,17],[1,5,18],[1,6,1],[1,6,2],[1,6,17],[1,6,18],[2,5,1],[2,5,2],[2,5,17],[2,5,18],[2,6,1],[2,6,2],[2,6,17],[2,6,18],[3,5,9],[3,5,10],[3,6,9],[3,6,10],[4,5,9],[4,5,10],[4,6,9],[4,6,10],[9,5,1],[9,5,2],[9,6,1],[9,6,2],[10,5,1],[10,5,2],[10,6,1],[10,6,2],[17,5,1],[17,5,2],[17,6,1],[17,6,2],[18,5,1],[18,5,2],[18,6,1],[18,6,2],[25,5,1],[25,5,2],[25,6,1],[25,6,2],[26,5,1],[26,5,2],[26,6,1],[26,6,2]],"consolidation_queue":[[0,1,4]],"edit_rejections":1,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5],[[78,46,14],[81,49,17],2,0.0],[[207,48,16],[209,49,17],4,0.0],[[30,46,78],[33,49,81],5,1.0]],"pending_edits":261,"pending_regions":[[0,1,0],[0,1,2],[0,1,4],[1,1,2],[2,1,0],[4,1,0],[6,1,0]]},"paint":{"colliders":[[1,5,1],[1,5,2],[1,6,1],[1,6,2],[2,5,1],[2,5,2],[2,6,1],[2,6,2],[9,5,1],[9,5,2],[9,6,1],[9,6,2],[10,5,1],[10,5,2],[10,6,1],[10,6,2],[17,5,1],[17,5,2],[17,6,1],[17,6,2],[18,5,1],[18,5,2],[18,6,1],[18,6,2]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5],[[78,46,14],[81,49,17],2,0.0]],"pending_edits":3,"pending_regions":[[0,1,0],[2,1,0],[4,1,0]]},"rejected":{"colliders":[[1,5,1],[1,5,2],[1,5,17],[1,5,18],[1,6,1],[1,6,2],[1,6,17],[1,6,18],[2,5,1],[2,5,2],[2,5,17],[2,5,18],[2,6,1],[2,6,2],[2,6,17],[2,6,18],[3,5,9],[3,5,10],[3,6,9],[3,6,10],[4,5,9],[4,5,10],[4,6,9],[4,6,10],[9,5,1],[9,5,2],[9,6,1],[9,6,2],[10,5,1],[10,5,2],[10,6,1],[10,6,2],[17,5,1],[17,5,2],[17,6,1],[17,6,2],[18,5,1],[18,5,2],[18,6,1],[18,6,2],[25,5,1],[25,5,2],[25,6,1],[25,6,2],[26,5,1],[26,5,2],[26,6,1],[26,6,2]],"consolidation_queue":[[0,1,4]],"edit_rejections":1,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5],[[78,46,14],[81,49,17],2,0.0],[[207,48,16],[209,49,17],4,0.0],[[30,46,78],[33,49,81],5,1.0]],"pending_edits":261,"pending_regions":[[0,1,0],[0,1,2],[0,1,4],[1,1,2],[2,1,0],[4,1,0],[6,1,0]]},"start":{"colliders":[],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[],"islands":[],"pending_edits":0,"pending_regions":[]},"subtract":{"colliders":[[1,5,1],[1,5,2],[1,6,1],[1,6,2],[2,5,1],[2,5,2],[2,6,1],[2,6,2]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5]],"pending_edits":1,"pending_regions":[[0,1,0]]},"volume":{"colliders":[[1,5,1],[1,5,2],[1,6,1],[1,6,2],[2,5,1],[2,5,2],[2,6,1],[2,6,2],[9,5,1],[9,5,2],[9,6,1],[9,6,2],[10,5,1],[10,5,2],[10,6,1],[10,6,2],[17,5,1],[17,5,2],[17,6,1],[17,6,2],[18,5,1],[18,5,2],[18,6,1],[18,6,2],[25,5,1],[25,5,2],[25,6,1],[25,6,2],[26,5,1],[26,5,2],[26,6,1],[26,6,2]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[],"islands":[[[14,46,14],[17,49,17],1,1.5],[[78,46,14],[81,49,17],2,0.0],[[207,48,16],[209,49,17],4,0.0]],"pending_edits":4,"pending_regions":[[0,1,0],[2,1,0],[4,1,0],[6,1,0]]}}
const FORCED_COMMIT_GOLDEN := {"after":{"colliders":[[0,4,0],[0,4,1],[0,4,2],[0,4,3],[0,5,0],[0,5,1],[0,5,2],[0,5,3],[0,6,0],[0,6,1],[0,6,2],[0,6,3],[0,7,0],[0,7,1],[0,7,2],[0,7,3],[1,4,0],[1,4,1],[1,4,2],[1,4,3],[1,5,0],[1,5,1],[1,5,2],[1,5,3],[1,6,0],[1,6,1],[1,6,2],[1,6,3],[1,7,0],[1,7,1],[1,7,2],[1,7,3],[2,4,0],[2,4,1],[2,4,2],[2,4,3],[2,5,0],[2,5,1],[2,5,2],[2,5,3],[2,6,0],[2,6,1],[2,6,2],[2,6,3],[2,7,0],[2,7,1],[2,7,2],[2,7,3],[3,4,0],[3,4,1],[3,4,2],[3,4,3],[3,5,0],[3,5,1],[3,5,2],[3,5,3],[3,6,0],[3,6,1],[3,6,2],[3,6,3],[3,7,0],[3,7,1],[3,7,2],[3,7,3]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[[0,1,0]],"islands":[[[8,46,14],[18,49,17],12,1.20000004768372]],"pending_edits":12,"pending_regions":[[0,1,0]]},"before":{"colliders":[[0,5,1],[0,5,2],[0,6,1],[0,6,2],[1,5,1],[1,5,2],[1,6,1],[1,6,2],[2,5,1],[2,5,2],[2,6,1],[2,6,2]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[],"islands":[[[8,46,14],[18,49,17],12,1.20000004768372]],"pending_edits":12,"pending_regions":[[0,1,0]]}}
const ASYNC_COMMIT_GOLDEN := {"after":{"colliders":[[0,4,0],[0,4,1],[0,4,2],[0,4,3],[0,5,0],[0,5,1],[0,5,2],[0,5,3],[0,6,0],[0,6,1],[0,6,2],[0,6,3],[0,7,0],[0,7,1],[0,7,2],[0,7,3],[1,4,0],[1,4,1],[1,4,2],[1,4,3],[1,5,0],[1,5,1],[1,5,2],[1,5,3],[1,6,0],[1,6,1],[1,6,2],[1,6,3],[1,7,0],[1,7,1],[1,7,2],[1,7,3],[2,4,0],[2,4,1],[2,4,2],[2,4,3],[2,5,0],[2,5,1],[2,5,2],[2,5,3],[2,6,0],[2,6,1],[2,6,2],[2,6,3],[2,7,0],[2,7,1],[2,7,2],[2,7,3],[3,4,0],[3,4,1],[3,4,2],[3,4,3],[3,5,0],[3,5,1],[3,5,2],[3,5,3],[3,6,0],[3,6,1],[3,6,2],[3,6,3],[3,7,0],[3,7,1],[3,7,2],[3,7,3]],"consolidation_queue":[],"edit_rejections":0,"forced_regen":[[0,1,0]],"islands":[[[8,46,14],[18,49,17],192,0.899999976158142]],"pending_edits":192,"pending_regions":[[0,1,0]]},"before":{"colliders":[[1,5,1],[1,5,2],[1,6,1],[1,6,2],[2,5,1],[2,5,2],[2,6,1],[2,6,2]],"consolidation_queue":[[0,1,0]],"edit_rejections":0,"forced_regen":[],"islands":[[[8,46,14],[18,49,17],192,0.899999976158142]],"pending_edits":192,"pending_regions":[[0,1,0]]}}
const LOD_GOLDEN := {"consolidated":[77,8],"edit":[28,7]}

# The LoD world settles over hundreds of ticks; these are test_lod_stream.gd's numbers.
# Trees moved the worst-case measured convergence to ~2519 ticks at the suite cameras
# (op_overflow=0, clean quiet streak); the budget is margin over measured ticks.
const SETTLE_BUDGET := 3500
const QUIET_TICKS := 8

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func own(w: VoxelWorld) -> VoxelWorld:
	add_child(w)
	_worlds.append(w)
	return w

# Every op below is at y = 38.4, where the golden pipeline has at least 2.4 m of rock, and
# each sits in the interior of its own 25.6 m region: x = 12.8 / 64.0 / 115.2 / 166.4 are
# regions 0 / 2 / 4 / 6 and z = 12.8 / 64.0 / 115.2 are regions 0 / 2 / 4, so every chunk
# range stays inside its region's own. The ops are two regions apart, so no two collider
# queue entries overlap and merging them cannot move an expanded chunk set.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	own(w)
	w.ensure_initialized()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func make_lod_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0
	w.max_lod_pages = 32768
	own(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

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

func check(name: String, measured: Dictionary, golden: Dictionary) -> void:
	print(name, " ", JSON.stringify(measured))
	assert_bool(golden.is_empty()).override_failure_message(
		"no golden recorded: paste the %s line above into the const" % name).is_false()
	assert_str(JSON.stringify(measured)).override_failure_message(
		"%s moved" % name).is_equal(JSON.stringify(golden))

# A small valid volume for the kOpVolumeAdd row.
func dummy_volume(dim: int) -> PackedByteArray:
	var a := PackedByteArray()
	a.resize(dim * dim * dim)
	a.fill(0)
	return a

# One op at a time, each read straight after, so a row can be attributed to its op. No await:
# nothing else runs between an edit and its read.
func test_each_edit_reaches_its_consumers(timeout := 300000) -> void:
	var w := make_world()
	var h := w.hooks()
	h.debug_stream_region(Vector3i(0, 1, 0))
	var measured := {}
	measured["start"] = h.debug_edit_fanout()
	h.debug_apply_sphere_subtract(Vector3(12.8, 38.4, 12.8), 1.5)
	measured["subtract"] = h.debug_edit_fanout()
	h.debug_apply_sphere_add(Vector3(64.0, 38.4, 12.8), 1.5, 1)
	measured["add"] = h.debug_edit_fanout()
	h.debug_apply_sphere_paint(Vector3(115.2, 38.4, 12.8), 1.5, 4)
	measured["paint"] = h.debug_edit_fanout()
	h.debug_store_volume(7, dummy_volume(4), dummy_volume(4), 4)
	h.debug_apply_volume_add(7, Vector3(166.4, 38.4, 12.8), 0.5, 4)
	measured["volume"] = h.debug_edit_fanout()
	# Region (0, 1, 0) ends at x = 25.6: this one lands in two regions.
	h.debug_apply_sphere_subtract(Vector3(25.6, 38.4, 64.0), 1.0)
	measured["border"] = h.debug_edit_fanout()
	# Fill one region's op list: the 192nd op queues the region for consolidation, and the
	# 257th is rejected.
	for i in range(256):
		h.debug_apply_sphere_paint(Vector3(12.8, 38.4, 115.2), 0.1, 4)
	measured["fill"] = h.debug_edit_fanout()
	h.debug_apply_sphere_paint(Vector3(12.8, 38.4, 115.2), 0.1, 4)
	measured["rejected"] = h.debug_edit_fanout()
	# Beyond ve::kMaxOpRegionSpan: refused outright, nothing touched.
	h.debug_apply_sphere_subtract(Vector3(0.0, 38.4, 300.0), 5000.0)
	measured["oversized"] = h.debug_edit_fanout()
	check("EDITS_GOLDEN", measured, EDITS_GOLDEN)

# The synchronous commit (ConsolidationCoordinator::force_region).
func test_a_forced_consolidation_reaches_its_consumers(timeout := 300000) -> void:
	var w := make_world()
	var h := w.hooks()
	h.debug_stream_region(Vector3i(0, 1, 0))
	for i in range(12):
		h.debug_apply_sphere_subtract(Vector3(8.0 + float(i) * 0.5, 38.4, 12.8), 1.2)
	var measured := {}
	measured["before"] = h.debug_edit_fanout()
	assert_bool(h.debug_consolidate_region(Vector3i(0, 1, 0))).override_failure_message(
		"the forced consolidation refused; the fixture is wrong, not the code").is_true()
	measured["after"] = h.debug_edit_fanout()
	check("FORCED_COMMIT_GOLDEN", measured, FORCED_COMMIT_GOLDEN)

# The asynchronous commit (ConsolidationCoordinator::pump_async), reached by filling a region
# to the 192-op queue threshold.
func test_an_async_consolidation_reaches_its_consumers(timeout := 300000) -> void:
	var w := make_world()
	var h := w.hooks()
	h.debug_stream_region(Vector3i(0, 1, 0))
	for i in range(192):
		h.debug_apply_sphere_subtract(Vector3(8.0 + float(i % 12) * 0.5, 38.4, 12.8), 0.9)
	var measured := {}
	measured["before"] = h.debug_edit_fanout()
	var done := false
	for i in range(600):
		h.debug_pump_consolidation_async()
		if int(h.debug_stream_stats().get("consolidations", 0)) > 0:
			done = true
			break
		await get_tree().process_frame
	assert_bool(done).override_failure_message("the queued region never consolidated").is_true()
	measured["after"] = h.debug_edit_fanout()
	check("ASYNC_COMMIT_GOLDEN", measured, ASYNC_COMMIT_GOLDEN)

# LoD hears an edit and a consolidation. Its marks are only observable in a settled LoD world,
# because LodTree::mark_dirty only touches nodes that exist.
func test_lod_hears_edits_and_consolidations(timeout := 900000) -> void:
	var w := make_lod_world()
	w.set_effect_enabled("near_field", false) # build the fine levels near the edit too
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var surface: Dictionary = w.hooks().debug_raycast(Vector3(400.0, 180.0, 380.0), Vector3.DOWN)
	assert_bool(surface["hit"]).is_true()
	var at: Vector3 = surface["pos"]
	w.hooks().debug_apply_sphere_subtract(at, 8.0)
	w.hooks().debug_drain_invalidations()
	var after_edit := w.hooks().debug_lod_stats()
	var measured := {"edit": [after_edit["dirty_chunks"], after_edit["dirty_levels"]]}
	assert_bool(await settle(w, pos, fwd)).is_true()
	assert_int(w.hooks().debug_lod_stats()["dirty_chunks"]).override_failure_message(
		"the crater never finished rebuilding, so the consolidation row would be ambiguous"
		).is_equal(0)
	var region := Vector3i(floori(at.x / 25.6), floori(at.y / 25.6), floori(at.z / 25.6))
	w.hooks().debug_stream_region(region)
	assert_bool(w.hooks().debug_consolidate_region(region)).override_failure_message(
		"the crater's region did not consolidate; the fixture is wrong, not the code").is_true()
	w.hooks().debug_drain_invalidations()
	var after_commit := w.hooks().debug_lod_stats()
	measured["consolidated"] = [after_commit["dirty_chunks"], after_commit["dirty_levels"]]
	check("LOD_GOLDEN", measured, LOD_GOLDEN)
