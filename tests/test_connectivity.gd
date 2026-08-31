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
	# Connectivity tests deliberately exercise fail-soft full op lists. M7's async
	# consolidation would otherwise bake those lists into override bricks and clear them
	# before the re-merge/preflight runs, so give this suite a one-brick override pool that
	# cannot absorb a real region bake and leaves the op lists full.
	w.max_override_bricks = 1
	w.physics_radius_m = 30.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func tool_of(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

# Fill the edit-log region containing `at` with `count` paint ops (default kMaxRegionOps).
# Paint ops consume region capacity without changing the SDF, which lets a test force
# append_edit rejection (or near-cap preflight refusal) on the carve/paste that follows
# without disturbing the terrain shape.
func fill_region_ops(w: VoxelWorld, t: VoxelEditTool, at: Vector3, count := 256) -> void:
	for i in range(count):
		t.apply_sphere_paint(at, 0.1, 4)

# A small valid volume for filling the VolumeSet pool through the committed-upload test hook.
# dim=2 is the smallest VolumeSet accepts, so filling 63 slots costs almost nothing.
func dummy_volume_bytes(dim: int) -> PackedByteArray:
	var n := dim * dim * dim
	var a := PackedByteArray()
	a.resize(n)
	a.fill(128)
	return a

# One frame of everything: streaming (which fills the occupancy grid), collider maintenance
# and the island manager, in the order VoxelWorld::_process runs them.
func step(w: VoxelWorld, frames: int, center: Vector3 = CENTER) -> void:
	for i in range(frames):
		w.hooks().debug_stream_frame(center)
		w.hooks().debug_physics_frame(center)
		w.hooks().debug_island_frame(1.0 / 60.0, center)

# Steps until no extraction is in flight. An extraction that has not landed yet has not been
# refused yet either, so the window it came from is momentarily neither pending nor finished,
# and reading pending_windows in that gap says nothing about whether a refusal kept it. On an
# idle machine the extractions land well inside the fixed step() the atlas-full tests used to
# rely on; under a full-suite run they do not, which is exactly when those tests flaked.
func step_until_at_rest(w: VoxelWorld, frames := 600) -> Dictionary:
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(frames):
		if st["in_flight"] == 0:
			return st
		step(w, 1)
		st = w.hooks().debug_island_stats()
	return st

func solid_at(w: VoxelWorld, p: Vector3) -> bool:
	# A downward ray from just above the point: it hits at p if there is matter there.
	var hit: Dictionary = w.hooks().debug_raycast(p + Vector3(0, 0.6, 0), Vector3(0, -1, 0))
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

	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"nothing came loose: %s" % st).is_greater(0)
	# Production atlas-backed islands remain raymarched; creating a second mesh would
	# duplicate large islands and shade them twice.
	assert_int(st["live_atlas_islands"]).is_greater(0)
	assert_int(st["live_island_render_meshes"]).is_equal(0)
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
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).is_greater(0)
	var y0: float = st["lowest_body_y"]
	for i in range(60):
		await get_tree().physics_frame
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	assert_float(w.hooks().debug_island_stats()["lowest_body_y"]).override_failure_message(
		"the island did not fall").is_less(y0 - 0.2)
	# Spec section 5's guardrails hold whatever happens.
	assert_int(st["live_bodies"]).is_less_equal(64)
	assert_int(st["live_islands"]).is_less_equal(32)

func test_the_carve_leaves_no_zero_planes_in_the_static_field(timeout := 180000) -> void:
	# Regression for the reported "pillar top sticks on slivers" bug. A cell-aligned box
	# carve used to leave SDF == 0 exactly on its cell faces wherever the removed matter was
	# solid: a CSG difference can only force the field UP TO 0 there. Every cell-aligned
	# sampler -- the 0.1 m chunk-mesh lattice, the 5 cm brick lattice, the 0.4 m occupancy
	# probe -- contains those planes, reads the exact 0 as solid, and builds a razor wall
	# standing inside the carved region. The walls are the "slivers": they poke into the
	# island's box compound and the island vice-clamps on them instead of falling. The carve
	# must therefore eat a small clearance past its cells: the field AT a carve face, and a
	# centimetre beyond it, has to be air.
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0)
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message("no island: %s" % st).is_greater(0)
	var carved: PackedInt32Array = st["carved_boxes"]
	assert_int(carved.size() / 6).is_greater(0)
	# For every carve box, sample the OUTER skin of the box union: 1 cm beyond each face
	# plane (outside the named cells) and 1 cm inside it. Both must be air; the inside one
	# always was, the outside one is where the phantom wall used to start.
	var bad := 0
	var checked := 0
	for i in range(0, carved.size(), 6):
		var lo := Vector3(carved[i], carved[i + 1], carved[i + 2]) * 0.8
		var hi := Vector3(carved[i + 3] + 1, carved[i + 4] + 1, carved[i + 5] + 1) * 0.8
		for d in range(6):
			var n := Vector3((-1.0 if d == 0 else (1.0 if d == 1 else 0.0)),
				(-1.0 if d == 2 else (1.0 if d == 3 else 0.0)),
				(-1.0 if d == 4 else (1.0 if d == 5 else 0.0)))
			for su in range(4):
				for sv in range(4):
					var fu := (su + 0.5) / 4.0
					var fv := (sv + 0.5) / 4.0
					var p := Vector3.ZERO
					if d <= 1:
						p = Vector3(lo.x if d == 0 else hi.x, lerpf(lo.y, hi.y, fu), lerpf(lo.z, hi.z, fv))
					elif d <= 3:
						p = Vector3(lerpf(lo.x, hi.x, fu), lo.y if d == 2 else hi.y, lerpf(lo.z, hi.z, fv))
					else:
						p = Vector3(lerpf(lo.x, hi.x, fu), lerpf(lo.y, hi.y, fv), lo.z if d == 4 else hi.z)
					checked += 1
					if w.hooks().debug_field_sdf(p + n * 0.01) < 0.0:
						bad += 1
					if w.hooks().debug_field_sdf(p - n * 0.01) < 0.0:
						bad += 1
	prints("carve-face clearance probe: %d/%d samples solid" % [bad, checked * 2])
	assert_int(bad).override_failure_message(
		"solid matter starts at the carve faces: the zero-plane slivers are still there").is_equal(0)

func test_a_dense_sphere_cut_pillar_top_falls_clear_of_the_cut(timeout := 180000) -> void:
	# The reported case, end to end: a dense stack cut by a big sphere frees the top, and the
	# top must fall clear of the cut instead of clamping onto the carve-boundary slivers a
	# few centimetres below its birth pose.
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0)
	var t := tool_of(w)
	for i in range(8):
		t.apply_sphere_add(Vector3(PILLAR_X, PILLAR_BASE + 1.4 * i, PILLAR_Z), 2.1, 4)
	step(w, 120)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 4.9, PILLAR_Z), 3.0)
	step(w, 120)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_islands"]).override_failure_message(
		"the severed dense pillar top never became a body: %s" % st).is_equal(1)
	var y0: float = st["lowest_body_y"]
	for i in range(240):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_physics_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	assert_float(w.hooks().debug_island_stats()["lowest_body_y"]).override_failure_message(
		"the severed pillar top is stuck on the carve-boundary slivers").is_less(y0 - 1.0)

func test_a_rested_island_merges_back_into_the_terrain(timeout := 180000) -> void:
	var w := make_world()
	# Hold the body out of start_merges() until we have observed it sleeping for several
	# consecutive frames; only then lower the sleep threshold and let it merge.
	w.hooks().debug_set_merge_sleep_seconds(999.0)
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)

	# Wait for the body to fall and be reported asleep by the physics server.
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if int(st["sleeping_bodies"]) > 0:
			break
	assert_int(st["sleeping_bodies"]).override_failure_message(
		"the body never fell asleep: %s" % st).is_greater(0)

	# Require several consecutive sleeping frames so the merge starts from a settled rest pose.
	var consecutive := 0
	for i in range(30):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		consecutive = consecutive + 1 if int(st["sleeping_bodies"]) > 0 else 0
		if consecutive >= 5:
			break
	assert_int(consecutive).override_failure_message(
		"the body did not stay asleep for five consecutive frames: %s" % st).is_greater_equal(5)

	w.hooks().debug_set_merge_sleep_seconds(0.2)
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["islands_merged"] > 0:
			break
	assert_int(st["islands_merged"]).override_failure_message(
		"the island never merged back: %s" % st).is_greater(0)
	assert_int(st["live_bodies"]).is_equal(0)
	# Spec section 5: "Rubble permanently accumulates as terrain." The rock is somewhere on
	# the ground under where it fell, and the FIELD knows about it. Probe the exact xz where
	# the manager recorded the merge rather than the pillar's original xz, which can drift as
	# the body tumbles before settling.
	var merge_x: float = st.get("last_merge_x", PILLAR_X)
	var merge_z: float = st.get("last_merge_z", PILLAR_Z)
	var down: Dictionary = w.hooks().debug_raycast(Vector3(merge_x, 90.0, merge_z), Vector3(0, -1, 0))
	assert_bool(down["hit"]).is_true()
	assert_float((down["pos"] as Vector3).y).override_failure_message(
		"the merged rubble is not standing on the ground at %s: %s" % [Vector3(merge_x, 0, merge_z), st]
		).is_greater((st["ground_y"] as float) - 0.1)

func test_an_anchored_overhang_is_left_alone(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	# Undercut the pillar without severing it: a 0.5 m bite out of one side.
	t.apply_sphere_subtract(Vector3(PILLAR_X + 1.0, PILLAR_BASE + 2.0, PILLAR_Z), 0.7)
	step(w, 180)
	assert_int(w.hooks().debug_island_stats()["islands_spawned"]).override_failure_message(
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
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["connectivity_runs"]).is_greater(0)
	# One window covered all three, so the pillar top came off exactly once.
	assert_int(st["islands_spawned"]).is_between(1, 3)

func test_more_than_two_loose_components_eventually_all_spawn(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the spawned bodies from merging mid-test
	var t := tool_of(w)
	var xs := [PILLAR_X - 4.0, PILLAR_X, PILLAR_X + 4.0]
	for x in xs:
		build_pillar(w, t, x)
	# Sever all three in one frame so a single connectivity window labels three components.
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 360)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"a multi-component blast did not eventually spawn every piece: %s" % st
		).is_greater_equal(3)
	assert_int(st["pending_windows"]).override_failure_message(
		"the remainder window was never drained: %s" % st).is_equal(0)

func test_body_pool_holes_after_merges_do_not_count_against_the_cap(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the spawn phase from merging mid-test
	# Shrink the guardrail so four loose tops are enough to prove the slot-pool bug: with the
	# old bodies_.size() check, merged-away bodies leave holes and the pool still reads "full"
	# for ever. M7's lattice occupancy no longer manufactures the 1-cell cut crumbs main used
	# to count as a fourth component, so use four real pillars instead.
	w.hooks().debug_set_max_dynamic_bodies(3)
	var t := tool_of(w)
	var xs := [PILLAR_X - 4.0, PILLAR_X, PILLAR_X + 4.0, PILLAR_X + 8.0]
	for x in xs:
		build_pillar(w, t, x)
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	assert_int(w.hooks().debug_island_stats()["live_bodies"]).is_greater_equal(3)

	# Take the baseline HERE, while the pool is full and visibly turning work away. The
	# carves left remainder windows queued behind the cap, so there is already demand waiting
	# for a slot -- which is what makes the hole test below meaningful, and why the demand
	# must not be created after the merge: a queued window claims the freed slot within a few
	# frames, so a pillar built afterwards would find the pool full again through no fault of
	# the slot pool. (That is exactly what this test used to do, and it measured the refusal
	# of its own late pillar rather than the hole it meant to test.)
	var before: Dictionary = w.hooks().debug_island_stats()
	var before_spawned: int = before["islands_spawned"] + before["debris_spawned"]
	assert_int(before["refused_body_cap"]).override_failure_message(
		"nothing was being refused, so a freed slot proves nothing: %s" % before).is_greater(0)
	w.hooks().debug_set_merge_sleep_seconds(0.2)

	# Let at least one body sleep and merge back, leaving a hole in the slot pool. We do not
	# require all three to merge: a single hole is enough to show the old bodies_.size() check
	# would still read "full" (size 3) while the live count is below the cap.
	for i in range(1200):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		if w.hooks().debug_island_stats()["live_bodies"] < 3:
			break
	var merged_stats: Dictionary = w.hooks().debug_island_stats()
	assert_int(merged_stats["islands_merged"]).override_failure_message(
		"no body merged back to free a slot: %s" % merged_stats).is_greater_equal(1)
	assert_int(merged_stats["live_bodies"]).override_failure_message(
		"live bodies stayed at the cap after the merge window: %s" % merged_stats).is_less(3)

	# The hole must be usable. With the old bodies_.size() check the pool reads "full" for
	# ever once three bodies have existed, and the work queued above never gets in.
	#
	# Poll for the reuse rather than counting frames. step() runs as fast as the CPU allows
	# (~0.2 ms an iteration), so a fixed frame budget is really a wall-clock budget on work
	# that happens on the mesher worker: the queued window only gets its turn once the merge
	# resample in flight lands. Source-field normals made that resample ~5x more expensive
	# (10 ms -> 50 ms for a 64^3 lattice, measured), which pushed the reuse past a 240-step
	# budget without changing whether the hole is ever reused.
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(2000):
		if st["islands_spawned"] + st["debris_spawned"] > before_spawned:
			break
		step(w, 1)
		st = w.hooks().debug_island_stats()
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"a merged-away body left a hole the pool never reused: %s" % st).is_greater(
		before_spawned)


func test_full_body_cap_keeps_window_queued_until_capacity_frees(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	w.hooks().debug_set_max_dynamic_bodies(1)
	var t := tool_of(w)
	var xs := [PILLAR_X - 4.0, PILLAR_X]
	for x in xs:
		build_pillar(w, t, x)
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the cap was not hit by the first component: %s" % st).is_equal(1)
	assert_int(st["pending_windows"]).override_failure_message(
		"the second component was dropped instead of staying queued: %s" % st).is_greater(0)

	# Let the first body merge back. Once the cap frees, the queued window must drain and
	# spawn the second component; dropping it would leave the second piece attached forever.
	for i in range(1800):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["islands_spawned"] + st["debris_spawned"] >= 2:
			break
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"the queued second component never spawned after capacity freed: %s" % st).is_greater_equal(2)

func test_rejected_remerge_paste_keeps_the_body_alive(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the body from merging before the region is full
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)
	# Fill the region where the body will rest, then allow re-merge. The paste volume-add is
	# rejected; the body must stay a body and the merge counter must not advance.
	fill_region_ops(w, t, Vector3(PILLAR_X, PILLAR_BASE, PILLAR_Z))
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
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
	w.hooks().debug_stream_frame(CENTER)
	w.hooks().debug_physics_frame(CENTER)
	w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	fill_region_ops(w, t, top)
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"a rejected carve still spawned a body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a rejected carve created a body in a field that still has the rock: %s" % st).is_equal(0)
	assert_bool(solid_at(w, top)).override_failure_message(
		"a rejected carve left a field hole with no body: %s" % st).is_true()

func test_atlas_slot_full_refusal_retries_after_a_slot_frees(timeout := 180000) -> void:
	var w := make_world()
	for i in range(32):
		w.hooks().debug_set_atlas_slot_used(i, true)
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"an island spawned despite every atlas slot being full: %s" % st).is_equal(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"the atlas-full refusal dropped the originating window: %s" % st).is_greater(0)
	# Free one slot; the re-queued window must retry and eventually spawn the island.
	w.hooks().debug_set_atlas_slot_used(0, false)
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		# The carve itself enqueues a follow-up connectivity window; wait for that too so the
		# re-queued atlas window has provably been consumed. Also wait for in_flight == 0: a
		# landing extraction can enqueue another window after pending_windows briefly reads 0.
		if st["islands_spawned"] > 0 and st["in_flight"] == 0 and st["pending_windows"] == 0:
			break
	assert_int(st["islands_spawned"]).override_failure_message(
		"the atlas-full refusal was never retried after a slot freed: %s" % st).is_greater(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"the retried island did not become a body: %s" % st).is_greater(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"the retried window was never drained: %s" % st).is_equal(0)

func test_atlas_full_remainder_window_gets_retry_backoff(timeout := 180000) -> void:
	var w := make_world()
	for i in range(32):
		w.hooks().debug_set_atlas_slot_used(i, true)
	var t := tool_of(w)
	# 3 m spacing keeps the pillars separate while their cut windows still overlap, so one
	# connectivity window labels all three and the first pass re-queues a remainder.
	var xs := [PILLAR_X - 3.0, PILLAR_X, PILLAR_X + 3.0]
	for x in xs:
		build_pillar(w, t, x)
	# Sever all three in one frame so the first connectivity pass submits two and re-queues a
	# remainder window without a cooldown. When those extractions hit the full atlas, the
	# duplicate-match in queue_retry_window must apply backoff to that existing remainder.
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	var st: Dictionary = step_until_at_rest(w)
	assert_int(st["in_flight"]).override_failure_message(
		"extractions never landed, so the refusal has not happened yet: %s" % st).is_equal(0)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"an island spawned despite every atlas slot being full: %s" % st).is_equal(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"the atlas-full refusal lost the originating window: %s" % st).is_greater(0)
	# Without applying the cooldown to the existing remainder, the same connectivity window is
	# relabelled as soon as the previous extractions land, so connectivity_runs grows almost
	# every frame. The fixed backoff should keep it bounded while the atlas stays full.
	assert_int(st["connectivity_runs"]).override_failure_message(
		"atlas-full remainder was relabelled without a retry cooldown: %s" % st).is_less(10)

func test_overlapping_edit_keeps_remainder_identity_for_retry_backoff(timeout := 180000) -> void:
	var w := make_world()
	for i in range(32):
		w.hooks().debug_set_atlas_slot_used(i, true)
	var t := tool_of(w)
	# 3 m spacing keeps the pillars separate while their cut windows still overlap, so one
	# connectivity window labels all of them and the first pass re-queues a remainder.
	var xs := [PILLAR_X - 6.0, PILLAR_X - 3.0, PILLAR_X, PILLAR_X + 3.0, PILLAR_X + 6.0]
	for x in xs:
		build_pillar(w, t, x)
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Run island frames until the first connectivity pass submits extractions and leaves the
	# unsubmitted tail as a remainder window, then stop immediately before those extractions
	# land on a later frame.
	var st: Dictionary = {}
	for i in range(120):
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_physics_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["in_flight"] > 0:
			break
	assert_int(st["in_flight"]).override_failure_message(
		"the first connectivity pass did not submit extractions: %s" % st).is_greater(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"the first connectivity pass did not leave a remainder: %s" % st).is_greater(0)
	var runs_before: int = st["connectivity_runs"]
	# A new edit overlapping that queued remainder merges into it, mutating lo/hi/seq. The
	# in-flight extractions still carry the old window identity; matching by stable id must
	# apply the atlas-full retry backoff to the merged window rather than pushing a duplicate.
	t.apply_sphere_subtract(Vector3(PILLAR_X + 0.2, PILLAR_BASE + 2.2, PILLAR_Z), 1.2)
	step(w, 120)
	st = step_until_at_rest(w)
	assert_int(st["in_flight"]).override_failure_message(
		"extractions never landed, so the refusal has not happened yet: %s" % st).is_equal(0)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"an island spawned despite every atlas slot being full: %s" % st).is_equal(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"the atlas-full refusal lost the merged window: %s" % st).is_greater(0)
	assert_int(st["connectivity_runs"] - runs_before).override_failure_message(
		"the merged remainder lost its identity and was relabelled without backoff: %s" % st
		).is_less(10)

func test_sleeping_body_can_remerge_when_volume_pool_is_full(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the body from merging before the pool is full
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)
	# Fill every remaining volume slot with pinned dummy volumes. The body owns one slot, so
	# allocate() can no longer find a second slot; start_merges must fall back to reusing the
	# body's own birth slot or the 64-body cap becomes a permanent merge deadlock.
	for slot in range(1, 64):
		w.hooks().debug_queue_committed_field_volume_upload(
				slot, dummy_volume_bytes(2), dummy_volume_bytes(2), 2)
	st = w.hooks().debug_island_stats()
	assert_int(st["volume_live"]).override_failure_message(
		"the volume pool was not filled: %s" % st).is_equal(64)
	var merged_before: int = st["islands_merged"]
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	for i in range(1200):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["islands_merged"] > merged_before:
			break
	assert_int(st["islands_merged"]).override_failure_message(
		"a sleeping body could not re-merge with the volume pool full: %s" % st
		).is_greater(merged_before)

func test_permanently_unavailable_extraction_does_not_relabel_remainder(timeout := 120000) -> void:
	var w := make_world()
	w.hooks().debug_set_extraction_available(false)
	var t := tool_of(w)
	# 3 m spacing keeps the pillars separate while their cut windows still overlap, so one
	# connectivity window labels three components and the first pass would ordinarily submit
	# two and re-queue a remainder. With extraction permanently unavailable, that remainder
	# must be dropped rather than relabelled and resubmitted every frame.
	var xs := [PILLAR_X - 3.0, PILLAR_X, PILLAR_X + 3.0]
	for x in xs:
		build_pillar(w, t, x)
	var runs_before: int = w.hooks().debug_island_stats()["connectivity_runs"]
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["connectivity_runs"] - runs_before).override_failure_message(
		"permanently unavailable extraction relabelled a remainder every frame: %s" % st
		).is_less(10)
	assert_int(st["pending_windows"]).override_failure_message(
		"permanently unavailable extraction kept a remainder window queued: %s" % st).is_equal(0)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"an extraction spawned despite being permanently unavailable: %s" % st).is_equal(0)

func test_persistent_extraction_failures_backoff_and_drop_remainder(timeout := 120000) -> void:
	var w := make_world()
	# The worker has a live IslandExtractPass, but every field extraction reports failure.
	# This exercises the per-window failure backoff/drop path rather than the no-pass path.
	w.hooks().debug_set_fail_extractions(true)
	var t := tool_of(w)
	var xs := [PILLAR_X - 3.0, PILLAR_X, PILLAR_X + 3.0]
	for x in xs:
		build_pillar(w, t, x)
	var runs_before: int = w.hooks().debug_island_stats()["connectivity_runs"]
	for x in xs:
		t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["connectivity_runs"] - runs_before).override_failure_message(
		"persistent extraction failures relabelled a remainder every frame: %s" % st
		).is_less(10)
	assert_int(st["pending_windows"]).override_failure_message(
		"persistent extraction failures never dropped/backed off the remainder: %s" % st
		).is_equal(0)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"an extraction spawned despite persistent extraction failures: %s" % st).is_equal(0)

func test_fully_submitted_extraction_failure_is_retried_and_dropped(timeout := 120000) -> void:
	var w := make_world()
	# A single component fits in one connectivity batch, so a failed field extraction has no
	# queued remainder to keep the edit alive. It must be re-queued with the same backoff/drop
	# policy, not silently dropped.
	w.hooks().debug_set_fail_extractions(true)
	var t := tool_of(w)
	build_pillar(w, t)
	var runs_before: int = w.hooks().debug_island_stats()["connectivity_runs"]
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(120):
		step(w, 1)
		st = w.hooks().debug_island_stats()
		if st["pending_windows"] > 0:
			break
	assert_int(st["pending_windows"]).override_failure_message(
		"a fully-submitted extraction failure was not re-queued: %s" % st).is_greater(0)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"an extraction spawned despite persistent extraction failures: %s" % st).is_equal(0)
	# Repeated failures reach the same drop threshold as a remainder window.
	step(w, 300)
	st = w.hooks().debug_island_stats()
	assert_int(st["connectivity_runs"] - runs_before).override_failure_message(
		"fully-submitted extraction failures relabelled without backoff: %s" % st).is_less(10)
	assert_int(st["pending_windows"]).override_failure_message(
		"fully-submitted extraction failures never dropped the window: %s" % st).is_equal(0)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"an extraction spawned despite persistent extraction failures: %s" % st).is_equal(0)

func test_resample_submit_colliding_with_in_flight_extractions_does_not_strand_merging(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the first body from merging before the collision
	var t := tool_of(w)
	build_pillar(w, t)
	build_pillar(w, t, PILLAR_X + 6.0, PILLAR_Z)
	# Sever the first pillar and let it spawn, fall and rest while merging is disabled.
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the first severed top never became a body: %s" % st).is_greater(0)
	var merged_before: int = st["islands_merged"]
	# Sever the second pillar and stop as soon as its field extraction is in flight.
	t.apply_sphere_subtract(Vector3(PILLAR_X + 6.0, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	for i in range(120):
		step(w, 1)
		st = w.hooks().debug_island_stats()
		if st["in_flight"] > 0:
			break
	assert_int(st["in_flight"]).override_failure_message(
		"the second extraction never entered flight: %s" % st).is_greater(0)
	# Now allow the first body to merge. run_frame will call start_merges while the second
	# extraction is still in flight; a rejected resample submit must not strand merging_.
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	w.hooks().debug_stream_frame(CENTER)
	w.hooks().debug_physics_frame(CENTER)
	w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	for i in range(1200):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["islands_merged"] > merged_before:
			break
	assert_int(st["islands_merged"]).override_failure_message(
		"a resample submit colliding with in-flight extractions stranded merging_: %s" % st
		).is_greater(merged_before)

func test_near_cap_carve_is_refused_before_any_carve(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the pillar was never built").is_true()
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Submit the extraction but do not let the result land yet; then bring the region to 255
	# ops. Accepting the carve would make it 256 and reject the restore volume-add, which used
	# to reach std::abort(). Preflight must refuse before any carve is appended.
	w.hooks().debug_stream_frame(CENTER)
	w.hooks().debug_physics_frame(CENTER)
	w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	fill_region_ops(w, t, top, 255)
	var refused_before: int = w.hooks().debug_island_stats()["refused"]
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["refused"]).override_failure_message(
		"the near-cap extraction was not refused: %s" % st).is_greater(refused_before)
	assert_int(st["islands_spawned"]).override_failure_message(
		"a near-cap carve still spawned a body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a near-cap carve created a body in a field that still has the rock: %s" % st).is_equal(0)
	assert_bool(solid_at(w, top)).override_failure_message(
		"a near-cap carve left a field hole with no body: %s" % st).is_true()

func test_cross_region_combined_op_count_is_refused_before_any_carve(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	# Put the pillar on the x region boundary (region 0 ends at 25.6 m), so its severed top
	# is a cross-region component. Each region individually stays under kMaxRegionOps, but
	# the flattened collector sees > 256 ops for the component and submit_extracts() must
	# refuse before allocating a slot or carving anything.
	var x := 24.8
	build_pillar(w, t, x)
	var top := Vector3(x, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the cross-region pillar was never built").is_true()
	# 200 paint ops on each side of the boundary. They consume op capacity but do not change
	# the SDF, and both points are inside the component's world AABB.
	fill_region_ops(w, t, Vector3(x - 0.5, PILLAR_BASE + 3.0, PILLAR_Z), 200)
	fill_region_ops(w, t, Vector3(x + 1.2, PILLAR_BASE + 3.0, PILLAR_Z), 200)
	t.apply_sphere_subtract(Vector3(x, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["refused"]).override_failure_message(
		"the cross-region over-cap extraction was not refused: %s" % st).is_greater(0)
	assert_int(st["islands_spawned"]).override_failure_message(
		"a cross-region over-cap extraction still spawned a body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a cross-region over-cap extraction created a body: %s" % st).is_equal(0)
	assert_bool(solid_at(w, top)).override_failure_message(
		"a cross-region over-cap extraction left a field hole with no body: %s" % st).is_true()

func test_stale_extraction_is_refused_before_any_carve(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the pillar was never built").is_true()
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Submit the extraction but do not let the result land yet; then change the field inside
	# the component's AABB. The captured ops no longer match the current field, so landing the
	# extraction must be refused under the same edit lock that guards the carve -- never
	# carving a stale volume into a field that has moved on.
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(120):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_physics_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["in_flight"] > 0:
			break
	assert_int(st["in_flight"]).override_failure_message(
		"the connectivity pass did not submit an extraction: %s" % st).is_greater(0)
	t.apply_sphere_subtract(top, 0.3)
	var refused_before: int = st["refused"]
	# Keep stepping until the stale result is collected and refused. The retry backoff after
	# a stale refusal prevents the newly queued window from carving in the same frame, so the
	# assertions below still catch the no-carve outcome before any replacement spawn lands.
	for i in range(120):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_physics_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["refused"] > refused_before:
			break
	assert_int(st["refused"]).override_failure_message(
		"a stale extraction was not refused: %s" % st).is_greater(refused_before)
	assert_int(st["islands_spawned"]).override_failure_message(
		"a stale extraction still spawned a body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a stale extraction created a body in a field that still has the rock: %s" % st).is_equal(0)


func test_failed_resample_backs_off_instead_of_retrying_every_frame(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the body from merging before the test is ready
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)
	var refused_before: int = st["refused"]
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	w.hooks().debug_set_fail_next_resample(true)
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["refused"] > refused_before:
			break
	assert_int(st["refused"]).override_failure_message(
		"the failed resample was not recorded: %s" % st).is_greater(refused_before)
	# The merge-retry cooldown must keep the body out of start_merges for ~30 frames. If the
	# failure path did not register a retry, the next frame would resample successfully and
	# merge immediately.
	for i in range(25):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
	assert_int(st["islands_merged"]).override_failure_message(
		"a failed resample merged before the retry cooldown elapsed: %s" % st).is_equal(0)
	# Once the cooldown expires, the body is allowed to try again and should merge normally.
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["islands_merged"] > 0:
			break
	assert_int(st["islands_merged"]).override_failure_message(
		"the body never merged after the resample backoff: %s" % st).is_greater(0)

func test_stale_rest_pose_resample_is_refused_before_paste(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the body from merging until the test is ready
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)
	var refused_before: int = st["refused"]
	var merged_before: int = st["islands_merged"]

	# Let the body sleep, then lower the threshold and run until a re-merge resample is
	# actually in flight. `merging` is the number of submitted resamples waiting to land.
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	for i in range(1200):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["merging"] > 0:
			break
	assert_int(st["merging"]).override_failure_message(
		"a re-merge resample was never submitted: %s" % st).is_greater(0)

	# The resample was captured at the body's rest pose. Offset and wake the body before the
	# worker result lands; the stale-rest-pose guard must refuse to paste the old pose and
	# must not despawn the body from its new pose.
	w.hooks().debug_offset_island_body(0, Vector3(0.25, 0.0, 0.0))
	for i in range(120):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["islands_merged"] > merged_before or st["refused"] > refused_before:
			break
	assert_int(st["islands_merged"]).override_failure_message(
		"a stale-rest-pose resample still pasted and despawned the body: %s" % st
		).is_equal(merged_before)
	assert_int(st["refused"]).override_failure_message(
		"the stale-rest-pose resample was not refused: %s" % st).is_greater(refused_before)
	assert_int(st["live_bodies"]).override_failure_message(
		"the stale-rest-pose resample despawned the body anyway: %s" % st).is_greater(0)

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
	w.hooks().debug_stream_frame(CENTER)
	w.hooks().debug_physics_frame(CENTER)
	w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	fill_region_ops(w, t, top)
	step(w, 120)
	var st: Dictionary = w.hooks().debug_island_stats()
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
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the body from merging before the region is full
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)
	# Fill every region the rest-volume AABB can touch, then allow re-merge. The paste must be
	# rejected everywhere, so no op references the pinned out-slot. It must be released, and
	# the body must not be retried every frame while the regions stay full.
	for y in [PILLAR_BASE - 4.0, PILLAR_BASE - 2.0, PILLAR_BASE, PILLAR_BASE + 2.0]:
		fill_region_ops(w, t, Vector3(PILLAR_X, y, PILLAR_Z))
	var volume_before: int = w.hooks().debug_island_stats()["volume_live"]
	var refused_before: int = w.hooks().debug_island_stats()["refused"]
	var field_uploads_before: int = w.hooks().debug_field_volume_upload_count()
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
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
	# The rejected paste queued a field-volume upload before append_edit rejected it. That
	# stale upload must be discarded before the debug frame drains the queue; otherwise it is
	# handed to the GPU and can later overwrite a reused slot.
	assert_int(w.hooks().debug_field_volume_upload_count()).override_failure_message(
		"a fully rejected re-merge paste still uploaded stale bytes for the released slot: %s"
		% st).is_equal(field_uploads_before)
	assert_int(st["refused"]).override_failure_message(
		"a fully rejected re-merge paste retried every frame: %s" % st).is_less(refused_before + 10)

func test_partially_rejected_remerge_preflight_never_corrupts_reused_birth_slot(timeout := 180000) -> void:
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0) # keep the body from merging before the pool is full
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 180)
	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["live_bodies"]).override_failure_message(
		"the severed top never became a body: %s" % st).is_greater(0)

	# Fill every remaining volume slot so start_merges() is forced to reuse the body's own
	# birth slot as the resample out-slot.
	for slot in range(1, 64):
		w.hooks().debug_queue_committed_field_volume_upload(
				slot, dummy_volume_bytes(2), dummy_volume_bytes(2), 2)
	st = w.hooks().debug_island_stats()
	assert_int(st["volume_live"]).override_failure_message(
		"the volume pool was not filled: %s" % st).is_equal(64)
	var volume_before: int = st["volume_live"]
	var pinned_before: int = st["volume_pinned"]

	# Fill only one of the paste regions. The rest volume spans more than one region, so a
	# naive append would be partially accepted and partially rejected; the fixed land_resample
	# must preflight-refuse before storing/pinning into the reused birth slot.
	fill_region_ops(w, t, Vector3(PILLAR_X, st["lowest_body_y"], PILLAR_Z))
	var refused_before: int = w.hooks().debug_island_stats()["refused"]
	w.hooks().debug_set_merge_sleep_seconds(0.2)
	for i in range(600):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["refused"] > refused_before or st["islands_merged"] > 0:
			break
	assert_int(st["refused"]).override_failure_message(
		"the partially-rejected re-merge was never preflight-refused: %s" % st
		).is_greater(refused_before)
	assert_int(st["islands_merged"]).override_failure_message(
		"a partially-rejected re-merge paste despawned the body: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a partially-rejected re-merge paste destroyed the body: %s" % st).is_greater(0)
	# The birth slot was never overwritten or pinned by the rejected attempt: volume_live and
	# volume_pinned are exactly what they were before the re-merge was allowed to run.
	assert_int(st["volume_live"]).override_failure_message(
		"partially-rejected preflight changed the volume pool: %s" % st).is_equal(volume_before)
	assert_int(st["volume_pinned"]).override_failure_message(
		"partially-rejected preflight pinned the body's birth slot: %s" % st).is_equal(pinned_before)

func test_failed_spawn_before_carve_leaves_component_attached(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Submit the extraction, then force the next spawn to fail before its result lands. The
	# structural fix spawns BEFORE carving, so a spawn failure must leave the terrain intact
	# (no carve, no hole) and the component still attached. Stop as soon as the refusal is
	# recorded, before any follow-up connectivity window can spawn the component again.
	w.hooks().debug_stream_frame(CENTER)
	w.hooks().debug_physics_frame(CENTER)
	w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	var refused_before: int = w.hooks().debug_island_stats()["refused"]
	w.hooks().debug_set_fail_next_spawn(true)
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(120):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["refused"] > refused_before:
			break
	assert_int(st["refused"]).override_failure_message(
		"spawn failure was not recorded as a refusal: %s" % st).is_greater(refused_before)
	assert_int(st["islands_spawned"]).override_failure_message(
		"a failed spawn was still counted as spawned: %s" % st).is_equal(0)
	assert_int(st["live_bodies"]).override_failure_message(
		"a failed spawn left a body behind: %s" % st).is_equal(0)
	assert_int(st["volume_pinned"]).override_failure_message(
		"a pre-carve spawn failure left the birth volume pinned: %s" % st).is_equal(0)
	assert_bool(solid_at(w, top)).override_failure_message(
		"a pre-carve spawn failure left a field hole with no body: %s" % st).is_true()

func test_rejected_extract_submit_rolls_back_in_flight_and_recovers(timeout := 180000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	var runs_before: int = w.hooks().debug_island_stats()["connectivity_runs"]
	# Force submit_extracts() to reject the batch even though the worker is idle. The manager
	# must roll back the InFlight entries it pushed and release their volume slots, then keep
	# the window alive so connectivity can succeed once the hook is cleared.
	w.hooks().debug_set_fail_extract_submit(true)
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(120):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_physics_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["connectivity_runs"] > runs_before:
			break
	assert_int(st["connectivity_runs"]).override_failure_message(
		"rejected-submit connectivity never ran: %s" % st).is_greater(runs_before)
	assert_int(st["in_flight"]).override_failure_message(
		"rejected extract submit stranded in-flight entries: %s" % st).is_equal(0)
	assert_int(st["volume_live"]).override_failure_message(
		"rejected extract submit leaked volume slots: %s" % st).is_equal(0)
	assert_int(st["pending_windows"]).override_failure_message(
		"rejected extract submit did not keep the window alive: %s" % st).is_greater(0)
	# Once submits are accepted again, the same edit must still produce an island.
	w.hooks().debug_set_fail_extract_submit(false)
	for i in range(240):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_physics_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["islands_spawned"] > 0:
			break
	assert_int(st["islands_spawned"]).override_failure_message(
		"rejected extract submit permanently lost the edit: %s" % st).is_greater(0)
	assert_int(st["in_flight"]).override_failure_message(
		"post-recovery extraction stranded in-flight entries: %s" % st).is_equal(0)

func test_post_spawn_carve_rejection_keeps_body_in_hole(timeout := 180000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	# Submit the extraction, then force the next carve to look rejected after at least one box
	# was accepted and force its restore to appear incomplete. The structural fix spawns the
	# body BEFORE carving, so this must leave the already-live body in the hole instead of
	# despawned; because the restore volume-add was accepted (touched non-empty), the birth
	# slot is referenced by the edit log and must remain pinned.
	w.hooks().debug_stream_frame(CENTER)
	w.hooks().debug_physics_frame(CENTER)
	w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
	w.hooks().debug_set_fail_next_carve(true)
	w.hooks().debug_set_fail_next_restore(true)
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(120):
		await get_tree().physics_frame
		w.hooks().debug_stream_frame(CENTER)
		w.hooks().debug_island_frame(1.0 / 60.0, CENTER)
		st = w.hooks().debug_island_stats()
		if st["live_bodies"] > 0:
			break
	assert_int(st["live_bodies"]).override_failure_message(
		"post-spawn carve rejection despawned the body into a hole: %s" % st).is_greater(0)
	assert_int(st["islands_spawned"]).override_failure_message(
		"post-spawn carve rejection body was not counted as spawned: %s" % st).is_greater(0)
	assert_int(st["volume_pinned"]).override_failure_message(
		"partial restore referenced the birth volume but it was unpinned: %s" % st).is_greater(0)

# A component the extractor cannot represent must not be left standing.
#
# ve::plan_island_lattice drops to the 10 cm pitch for any component wider than 2.95 m, while
# the occupancy grid the labeller reads is a conservative test over the 5 cm brick lattice. A
# sheet thinner than the pitch therefore reads SOLID to the labeller and EMPTY to the
# extractor -- which is exactly what a sphere carve through a sphere-add pillar leaves behind:
# paper-thin dishes a few centimetres thick.
#
# Leaving one in place was the old behaviour, and it is the worst of the three outcomes: the
# matter stays STATIC inside the space the freed piece was cut out of, so the piece wedges
# against it instead of falling, and every later connectivity run re-labels and re-extracts
# the same cells for ever.
#
# The subject is a small ball floating clear of the terrain: unanchored, and small enough
# that no cell of it is kCellFull, which is the shape the crumble is allowed to act on. The
# empty landing is forced with the hook rather than carved by hand, because no fixed set of
# tool calls produces a sub-pitch sliver reliably across the analytic hills.
func test_a_component_too_thin_to_extract_is_carved_away_not_left_standing(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	# Dead centre of occupancy cell (25, 77, 25) -- (floor(v / 0.8) + 0.5) * 0.8 -- with a
	# radius small enough that the whole ball lies inside that one cell. One cell means the
	# carve accounts for the whole ball, and a ball smaller than its cell means the cell is
	# kCellSolid rather than kCellFull, which is the shape a crumble is allowed to act on.
	var speck := Vector3(20.4, 62.0, 20.4)
	var elsewhere := Vector3(24.4, 0.0, 24.4) # clear of the speck's cell
	t.apply_sphere_add(speck, 0.35, 4)
	step(w, 90)
	assert_bool(solid_at(w, speck)).override_failure_message(
		"the floating speck was never built").is_true()
	var ground_before: Dictionary = w.hooks().debug_raycast(
		Vector3(elsewhere.x, 90.0, elsewhere.z), Vector3(0, -1, 0))
	assert_bool(ground_before["hit"]).is_true()

	# The next extraction to land reports no solid sample, as a sub-pitch sheet does.
	w.hooks().debug_set_empty_next_extraction(true)
	# An edit near it, so connectivity has a window to act on; it misses the speck itself.
	t.apply_sphere_subtract(speck + Vector3(0.0, -1.6, 0.0), 0.4)
	step(w, 240)

	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["crumbled"]).override_failure_message(
		"the unrepresentable component was not crumbled: %s" % st).is_greater(0)
	assert_int(st["refused_empty"]).override_failure_message(
		"the crumble was refused: %s" % st).is_equal(0)
	# The whole point: the matter is GONE from the static field. Before the fix it stayed --
	# and stayed labelled, extracted and dropped again on every later connectivity run.
	assert_bool(solid_at(w, speck)).override_failure_message(
		"the component the extractor could not represent is still in the terrain: %s" % st
		).is_false()
	assert_int(w.hooks().debug_cell_state(Vector3i(25, 77, 25))).override_failure_message(
		"the crumbled cell is still marked solid, so connectivity will label it again"
		).is_equal(1) # ve::kCellAir
	# ...and nothing outside the component went with it.
	var ground_after: Dictionary = w.hooks().debug_raycast(
		Vector3(elsewhere.x, 90.0, elsewhere.z), Vector3(0, -1, 0))
	assert_bool(ground_after["hit"]).override_failure_message(
		"crumbling removed the terrain beside the component").is_true()
	assert_float((ground_after["pos"] as Vector3).y).is_equal_approx(
		(ground_before["pos"] as Vector3).y, 0.01)

# The corollary: a crumble must not be able to eat solid rock. A cell the mark pass called
# kCellFull holds no air sample at 5 cm, so an extraction that finds nothing solid inside one
# is a disagreement no thin sheet can explain -- the component is left alone and counted
# under refused_empty instead.
func test_a_solid_component_is_never_crumbled(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).is_true()

	# The severed top is solid through and through: its inner cells hold no air sample at all.
	w.hooks().debug_set_empty_next_extraction(true)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)

	var st: Dictionary = w.hooks().debug_island_stats()
	assert_int(st["refused_empty"]).override_failure_message(
		"a fully solid component was not protected from the crumble: %s" % st).is_greater(0)
	# It is still there: better a piece that has to wait for the next connectivity run than
	# a crumble that deletes rock on the strength of a disagreement it cannot explain.
	assert_bool(solid_at(w, Vector3(PILLAR_X, PILLAR_BASE, PILLAR_Z))).override_failure_message(
		"the crumble ate the stump").is_true()
