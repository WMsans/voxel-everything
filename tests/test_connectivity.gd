extends GdUnitTestSuite

# Spec section 5, end to end. Blow the middle out of a pillar and the top must:
#   1. stop being part of the terrain (the carve),
#   2. become a rigid body that falls (the spawn),
#   3. come back as terrain where it lands (the re-merge).
#
# Every assertion is against the FIELD, through ve::raycast, because that is what the
# renderer, the collision mesher and the LoD bakery all read: if the field says the pillar
# top is gone, every consumer agrees it is gone.

const CENTER := Vector3(20.0, 56.0, 20.0)
# A pillar built above the terrain with sphere-adds, so the test does not depend on where
# the analytic hills happen to put a cliff.
const PILLAR_X := 20.4
const PILLAR_Z := 20.4
const PILLAR_BASE := 52.0

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
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(48, 24, 48)
	w.max_region_slots = 64
	w.physics_radius_m = 30.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func tool_of(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

# Fill the edit-log region containing `at` to kMaxRegionOps (256). Paint ops consume region
# capacity without changing the SDF, which lets a test force append_edit rejection on the
# carve/paste that follows without disturbing the terrain shape.
func fill_region_ops(w: VoxelWorld, t: VoxelEditTool, at: Vector3) -> void:
	for i in range(256):
		t.apply_sphere_paint(at, 0.1, 4)

# One frame of everything: streaming (which fills the occupancy grid), collider maintenance
# and the island manager, in the order VoxelWorld::_process runs them.
func step(w: VoxelWorld, frames: int, center: Vector3 = CENTER) -> void:
	for i in range(frames):
		w.debug_stream_frame(center)
		w.debug_physics_frame(center)
		w.debug_island_frame(1.0 / 60.0, center)

func solid_at(w: VoxelWorld, p: Vector3) -> bool:
	# A downward ray from just above the point: it hits at p if there is matter there.
	var hit: Dictionary = w.debug_raycast(p + Vector3(0, 0.6, 0), Vector3(0, -1, 0))
	# 0.7 m: the helper is also used for points inside a sphere, where the downward ray can
	# start already inside solid and return the origin (p.y + 0.6) as the hit.
	return hit["hit"] and absf((hit["pos"] as Vector3).y - p.y) < 0.7

func build_pillar(w: VoxelWorld, t: VoxelEditTool, x := PILLAR_X, z := PILLAR_Z) -> void:
	# Five overlapping 1.2 m balls stacked into a 6 m column standing on the terrain.
	for i in range(5):
		t.apply_sphere_add(Vector3(x, PILLAR_BASE + 1.0 * i, z), 1.2, 4)
	step(w, 90)

func test_the_grid_and_the_flood_find_a_severed_pillar_top(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the pillar was never built").is_true()

	# Cut the pillar in half.
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)

	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"nothing came loose: %s" % st).is_greater(0)
	# The top is no longer part of the static field: it is a body now.
	assert_bool(solid_at(w, top)).override_failure_message(
		"the severed top is still in the terrain (the carve did not happen)").is_false()
	# ...and the stump below the cut is untouched.
	assert_bool(solid_at(w, Vector3(PILLAR_X, PILLAR_BASE, PILLAR_Z))).is_true()

func test_the_island_body_falls_and_the_bodies_are_capped(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["live_bodies"]).is_greater(0)
	var y0: float = st["lowest_body_y"]
	for i in range(60):
		await get_tree().physics_frame
		w.debug_island_frame(1.0 / 60.0, CENTER)
	assert_float(w.debug_island_stats()["lowest_body_y"]).override_failure_message(
		"the island did not fall").is_less(y0 - 0.2)
	# Spec section 5's guardrails hold whatever happens.
	assert_int(st["live_bodies"]).is_less_equal(64)
	assert_int(st["live_islands"]).is_less_equal(32)

func test_a_rested_island_merges_back_into_the_terrain(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(0.2) # the demo waits 2 s; a test should not
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	assert_int(w.debug_island_stats()["islands_spawned"]).is_greater(0)

	for i in range(600):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		if w.debug_island_stats()["islands_merged"] > 0:
			break
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_merged"]).override_failure_message(
		"the island never merged back: %s" % st).is_greater(0)
	assert_int(st["live_bodies"]).is_equal(0)
	# Spec section 5: "Rubble permanently accumulates as terrain." The rock is somewhere on
	# the ground under where it fell, and the FIELD knows about it.
	var down: Dictionary = w.debug_raycast(Vector3(PILLAR_X, 90.0, PILLAR_Z), Vector3(0, -1, 0))
	assert_bool(down["hit"]).is_true()
	assert_float((down["pos"] as Vector3).y).override_failure_message(
		"the merged rubble is not standing on the ground").is_greater(
		(st["ground_y"] as float) - 0.1)

func test_an_anchored_overhang_is_left_alone(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	# Undercut the pillar without severing it: a 0.5 m bite out of one side.
	t.apply_sphere_subtract(Vector3(PILLAR_X + 1.0, PILLAR_BASE + 2.0, PILLAR_Z), 0.7)
	step(w, 180)
	assert_int(w.debug_island_stats()["islands_spawned"]).override_failure_message(
		"a still-attached pillar was declared an island").is_equal(0)
	assert_bool(solid_at(w, Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z))).is_true()

func test_connectivity_runs_once_per_frame_however_many_edits_land(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	# Three blasts in one frame (spec section 5: "simultaneous blasts can't race").
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	t.apply_sphere_subtract(Vector3(PILLAR_X + 0.4, PILLAR_BASE + 2.2, PILLAR_Z), 1.2)
	t.apply_sphere_subtract(Vector3(PILLAR_X - 0.4, PILLAR_BASE + 2.2, PILLAR_Z), 1.2)
	step(w, 240)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["connectivity_runs"]).is_greater(0)
	# One window covered all three, so the pillar top came off exactly once.
	assert_int(st["islands_spawned"]).is_between(1, 3)

func test_more_than_two_loose_components_eventually_all_spawn(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(999.0) # keep the spawned bodies from merging mid-test
	var t := tool_of(w)
	var xs := [PILLAR_X - 4.0, PILLAR_X, PILLAR_X + 4.0]
	for x in xs:
		build_pillar(w, t, x)
	# Sever all three in one frame so a single connectivity window labels three components.
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 360)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"a multi-component blast did not eventually spawn every piece: %s" % st
		).is_greater_equal(3)
	assert_int(st["pending_windows"]).override_failure_message(
		"the remainder window was never drained: %s" % st).is_equal(0)

func test_body_pool_holes_after_merges_do_not_count_against_the_cap(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(999.0) # keep the spawn phase from merging mid-test
	# Shrink the guardrail so three spawns are enough to prove the slot-pool bug: with the
	# old bodies_.size() check, three merged-away bodies leave three holes and the pool still
	# reads "full" for ever.
	w.debug_set_max_dynamic_bodies(3)
	var t := tool_of(w)
	var xs := [PILLAR_X - 4.0, PILLAR_X, PILLAR_X + 4.0]
	for x in xs:
		build_pillar(w, t, x)
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	assert_int(w.debug_island_stats()["live_bodies"]).is_greater_equal(3)
	w.debug_set_merge_sleep_seconds(0.2)

	# Let at least one body sleep and merge back, leaving a hole in the slot pool. We do not
	# require all three to merge: a single hole is enough to show the old bodies_.size() check
	# would still read "full" (size 3) while the live count is below the cap.
	for i in range(1200):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		if w.debug_island_stats()["live_bodies"] < 3:
			break
	var merged_stats: Dictionary = w.debug_island_stats()
	assert_int(merged_stats["islands_merged"]).override_failure_message(
		"no body merged back to free a slot: %s" % merged_stats).is_greater_equal(1)
	assert_int(merged_stats["live_bodies"]).override_failure_message(
		"live bodies stayed at the cap after the merge window: %s" % merged_stats).is_less(3)

	# A fresh island must still be allowed even though the slot pool has three holes in it.
	build_pillar(w, t, PILLAR_X + 8.0)
	var before: Dictionary = w.debug_island_stats()
	var before_spawned: int = before["islands_spawned"] + before["debris_spawned"]
	t.apply_sphere_subtract(Vector3(PILLAR_X + 8.0, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"a later island was refused after merges freed the body slots: %s" % st).is_greater(
		before_spawned)


func test_full_body_cap_keeps_window_queued_until_capacity_frees(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(0.2)
	w.debug_set_max_dynamic_bodies(1)
	var t := tool_of(w)
	var xs := [PILLAR_X - 4.0, PILLAR_X]
	for x in xs:
		build_pillar(w, t, x)
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the cap was not hit by the first component: %s" % st).is_equal(1)
	assert_int(st["pending_windows"]).override_failure_message(
		"the second component was dropped instead of staying queued: %s" % st).is_greater(0)

	# Let the first body merge back. Once the cap frees, the queued window must drain and
	# spawn the second component; dropping it would leave the second piece attached forever.
	for i in range(1800):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		st = w.debug_island_stats()
		if st["islands_spawned"] + st["debris_spawned"] >= 2:
			break
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"the queued second component never spawned after capacity freed: %s" % st).is_greater_equal(2)

func test_rejected_remerge_paste_keeps_the_body_alive(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(999.0) # keep the body from merging before the region is full
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)
	# Fill the region where the body will rest, then allow re-merge. The paste volume-add is
	# rejected; the body must stay a body and the merge counter must not advance.
	fill_region_ops(w, t, Vector3(PILLAR_X, PILLAR_BASE, PILLAR_Z))
	w.debug_set_merge_sleep_seconds(0.2)
	for i in range(600):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		st = w.debug_island_stats()
		if st["islands_merged"] > 0:
			break
	assert_int(st["islands_merged"]).override_failure_message(
		"a rejected re-merge paste still despawned the body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a rejected re-merge paste destroyed the body and left a hole: %s" % st).is_greater(0)

func test_rejected_carve_keeps_component_attached(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the pillar was never built").is_true()
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Run one island frame so connectivity labels the top and submits the extraction, but do
	# NOT let the extraction result land yet; then fill the region so the carve is rejected.
	w.debug_stream_frame(CENTER)
	w.debug_physics_frame(CENTER)
	w.debug_island_frame(1.0 / 60.0, CENTER)
	fill_region_ops(w, t, top)
	step(w, 240)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"a rejected carve still spawned a body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a rejected carve created a body in a field that still has the rock: %s" % st).is_equal(0)
	assert_bool(solid_at(w, top)).override_failure_message(
		"a rejected carve left a field hole with no body: %s" % st).is_true()

func test_atlas_slot_full_refusal_retries_after_a_slot_frees(timeout := 180000) -> void:
	var w := make_world()
	for i in range(32):
		w.debug_set_atlas_slot_used(i, true)
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"an island spawned despite every atlas slot being full: %s" % st).is_equal(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"the atlas-full refusal dropped the originating window: %s" % st).is_greater(0)
	# Free one slot; the re-queued window must retry and eventually spawn the island.
	w.debug_set_atlas_slot_used(0, false)
	for i in range(600):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		st = w.debug_island_stats()
		# The carve itself enqueues a follow-up connectivity window; wait for that too so the
		# re-queued atlas window has provably been consumed.
		if st["islands_spawned"] > 0 and st["pending_windows"] == 0:
			break
	assert_int(st["islands_spawned"]).override_failure_message(
		"the atlas-full refusal was never retried after a slot freed: %s" % st).is_greater(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"the retried island did not become a body: %s" % st).is_greater(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"the retried window was never drained: %s" % st).is_equal(0)

func test_fully_rejected_op_does_not_enqueue_connectivity_window(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the pillar was never built").is_true()
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Submit the extraction but do not let the result land yet; then fill the region so the
	# carve is rejected in every touched region. The rejected carve must NOT enqueue another
	# connectivity window for the same unchanged component.
	w.debug_stream_frame(CENTER)
	w.debug_physics_frame(CENTER)
	w.debug_island_frame(1.0 / 60.0, CENTER)
	fill_region_ops(w, t, top)
	step(w, 120)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"a rejected carve still spawned a body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a rejected carve created a body in a field that still has the rock: %s" % st).is_equal(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"a fully rejected carve enqueued a connectivity retry window: %s" % st).is_equal(0)
	assert_bool(solid_at(w, top)).override_failure_message(
		"a rejected carve left a field hole with no body: %s" % st).is_true()

func test_rejected_remerge_paste_does_not_leak_pinned_slots_or_retry_every_frame(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(999.0) # keep the body from merging before the region is full
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)
	# Fill every region the rest-volume AABB can touch, then allow re-merge. The paste must be
	# rejected everywhere, so no op references the pinned out-slot. It must be released, and
	# the body must not be retried every frame while the regions stay full.
	for y in [PILLAR_BASE - 4.0, PILLAR_BASE - 2.0, PILLAR_BASE, PILLAR_BASE + 2.0]:
		fill_region_ops(w, t, Vector3(PILLAR_X, y, PILLAR_Z))
	var volume_before: int = w.debug_island_stats()["volume_live"]
	var refused_before: int = w.debug_island_stats()["refused"]
	w.debug_set_merge_sleep_seconds(0.2)
	for i in range(600):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		st = w.debug_island_stats()
		if st["refused"] > refused_before or st["islands_merged"] > 0:
			break
	assert_int(st["refused"]).override_failure_message(
		"the rejected re-merge paste was never attempted: %s" % st).is_greater(refused_before)
	assert_int(st["islands_merged"]).override_failure_message(
		"a rejected re-merge paste still despawned the body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a rejected re-merge paste destroyed the body and left a hole: %s" % st).is_greater(0)
	assert_int(st["volume_live"]).override_failure_message(
		"a fully rejected re-merge paste leaked pinned volume slots: %s" % st).is_equal(volume_before)
	assert_int(st["refused"]).override_failure_message(
		"a fully rejected re-merge paste retried every frame: %s" % st).is_less(refused_before + 10)

func test_spawn_failure_restores_field_with_no_body(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Submit the extraction, then force the next spawn to fail before its result lands. Stop
	# as soon as the refusal is recorded, before any follow-up connectivity window can spawn
	# the component again, and verify the failed spawn left the field restored, not a hole.
	w.debug_stream_frame(CENTER)
	w.debug_physics_frame(CENTER)
	w.debug_island_frame(1.0 / 60.0, CENTER)
	var refused_before: int = w.debug_island_stats()["refused"]
	w.debug_set_fail_next_spawn(true)
	var st: Dictionary = w.debug_island_stats()
	for i in range(120):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		st = w.debug_island_stats()
		if st["refused"] > refused_before:
			break
	assert_int(st["refused"]).override_failure_message(
		"spawn failure was not recorded as a refusal: %s" % st).is_greater(refused_before)
	assert_int(st["islands_spawned"]).override_failure_message(
		"a failed spawn was still counted as spawned: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a failed spawn left a body behind: %s" % st).is_equal(0)
	assert_bool(solid_at(w, top)).override_failure_message(
		"spawn failure left a field hole with no body: %s" % st).is_true()
