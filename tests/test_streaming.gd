extends GdUnitTestSuite

# The streamer driving residency -> mark -> generate end to end on a shrunk atlas.
#
# Atlas sizing rule of thumb for every M2 test (keep this comment in sync):
# only regions CROSSING the surface hold bricks (~1500 bricks per shell region for the
# gentle analytic hills), and a residency ball of radius R holds ~2*pi*R^2/655 shell
# regions. R = 20 m -> ~4 shell regions -> ~6k bricks, so 16384 slots is comfortable.
const ATLAS := Vector3i(32, 16, 32)   # 16384 slots (~170 MB on the test device)
const REGION_SLOTS := 16
const ORIGIN := Vector3i(0, -64, 0)   # y regions {-2..2}: world y in [-51.2, 76.8) m
const REGIONS := Vector3i(4, 5, 4)
const RADIUS := 20.0
# Errata 9: the generator field now carries the +51.2 surface offset (ve::kSurfaceY), so
# the surface sits at 51.2 +- 10 m and the brief's camera/sweep constants are correct:
# the camera flies at y = 56.2 and the column sweep crosses the surface near brick 64.

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = ORIGIN
	w.world_size_regions = REGIONS
	w.residency_radius_m = RADIUS
	add_child(w)
	w.ensure_initialized()
	return w

# Settled means SEVERAL consecutive frames with nothing to do. One is not enough: the
# streamer paces stream-in against the atlas free count, which it reads back a frame or more
# behind the GPU, so it deliberately holds back while the loads it has already started are
# still charged against the budget. A quiet frame is that pause, not the end of the work —
# so wait out more frames than the streamer's in-flight window (WorldStreamer's four).
const SETTLE_QUIET_FRAMES := 6

func settle(w: VoxelWorld, cam: Vector3) -> void:
	var quiet := 0
	for i in range(120):
		quiet = quiet + 1 if w.debug_stream_frame(cam) == 0 else 0
		if quiet >= SETTLE_QUIET_FRAMES:
			return
	fail("streamer did not settle within 120 frames")

func test_streaming_loads_the_camera_neighbourhood() -> void:
	var w := make_world()
	settle(w, Vector3(20, 56.2, 20)) # 56.2 m: above the local surface (51.2 +- 10)
	var s: Dictionary = w.debug_stream_stats()
	assert_int(s["resident_regions"]).is_greater(4)
	assert_int(s["overflow_ever"]).is_equal(0)
	# The camera at (20, 56.2, 20) sits inside region (0, 2, 0) (world y [51.2, 76.8));
	# region (0, 1, 0) (world y [25.6, 51.2)) is 5 m below it and also resident.
	var rslot: int = w.debug_region_map_entry(Vector3i(0, 2, 0))
	assert_int(rslot).is_greater_equal(0)
	assert_int(w.debug_region_map_entry(Vector3i(0, 1, 0))).is_greater_equal(0)
	# The column at world (12.8, *, 12.8) crosses the surface near brick 64 (world y
	# 51.2): sweep bricks y in [56, 72) (world [44.8, 57.6) m) and require the GPU to hold
	# exactly the bricks the CPU probe calls active. Each swept brick is read from the
	# table of the region that OWNS it (y-regions {1, 2} here).
	var cpu_active := 0
	var gpu_match := 0
	for by in range(56, 72):
		var brick := Vector3i(16, by, 16)
		if w.debug_brick_has_surface(brick, PackedByteArray(), 0):
			cpu_active += 1
			var owner_slot: int = w.debug_region_map_entry(Vector3i(0, floori(by / 32.0), 0))
			if owner_slot >= 0 and w.debug_region_table_slot(owner_slot, brick) >= 0:
				gpu_match += 1
	assert_int(cpu_active).override_failure_message(
		"column holds no surface; check the world-origin maths").is_greater(0)
	assert_int(gpu_match).is_equal(cpu_active)

func test_moving_the_camera_streams_the_new_neighbourhood_and_recycles_slots() -> void:
	var w := make_world()
	settle(w, Vector3(20, 56.2, 20))
	var used_before := ATLAS.x * ATLAS.y * ATLAS.z - int(w.debug_atlas_stats()["free_slots"])
	assert_int(used_before).is_greater(0)
	assert_bool(w.debug_region_map_consistent()).is_true()

	settle(w, Vector3(90, 56.2, 90))
	# Region (0, -1, 0) (world x,z [0, 25.6), y [-25.6, 0)) is ~107 m from the new camera,
	# far past the 23 m evict boundary.
	assert_int(w.debug_slot_of_region(Vector3i(0, -1, 0))).is_equal(-1)
	assert_int(w.debug_region_map_entry(Vector3i(0, -1, 0))).is_equal(-1)
	# The new neighbourhood is resident and the residency core agrees with the GPU map:
	# the camera at (90, 56.2, 90) sits inside region (3, 2, 3) (x,z [76.8, 102.4),
	# y [51.2, 76.8)) — distance 0, so it must be resident.
	assert_int(w.debug_region_map_entry(Vector3i(3, 2, 3))).is_greater_equal(0)
	assert_bool(w.debug_region_map_consistent()).is_true()
	# Evicted regions returned their atlas slots: no leak, no overflow, no unbounded drop.
	var s: Dictionary = w.debug_stream_stats()
	assert_int(s["overflow_ever"]).is_equal(0)
	var free_now: int = w.debug_atlas_stats()["free_slots"]
	# used_now < used_before + slack: slack tolerates terrain-density variation between the
	# two spots; a leak of the whole old neighbourhood (~6k bricks) would blow past it.
	assert_int(free_now).is_greater(ATLAS.x * ATLAS.y * ATLAS.z - used_before - 2048)

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type); b.put_u32(material)
	b.put_float(pos.x); b.put_float(pos.y); b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0); b.put_u32(0)
	return b.data_array

func test_an_oversubscribed_atlas_keeps_slots_for_edits() -> void:
	# The demo's failure: at residency_radius_m = 96 m the surface shell demands ~140k
	# bricks against a 65536-slot atlas, so plain streaming drains the free list to zero.
	# Every later edit then hits the free-list-empty fail-soft in brick_mark.comp.glsl —
	# the bricks it activates are dropped for good, and the terrain around the edit
	# shatters into floating slabs. Streaming must cap the resident set at what the atlas
	# holds (nearest regions win) instead of spending the last slot.
	#
	# 8000 slots against the ~6k-brick shell of a 20 m ball is not oversubscribed on its
	# own, so this shrinks the atlas to 4000 and asks for the same ball.
	const SMALL_ATLAS := Vector3i(20, 10, 20) # 4000 slots vs a ~6k-brick demand
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = SMALL_ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = ORIGIN
	w.world_size_regions = REGIONS
	w.residency_radius_m = RADIUS
	add_child(w)
	w.ensure_initialized()
	var cam := Vector3(40, 56.2, 40)
	for i in range(120):
		if w.debug_stream_frame(cam) == 0:
			break

	# The atlas is over-subscribed, so some regions must stay unloaded — but the free list
	# must never bottom out, and no brick may be dropped.
	var stats: Dictionary = w.debug_atlas_stats()
	assert_int(int(stats["free_slots"])).override_failure_message(
		"streaming drained the atlas free list to %d: an edit can no longer allocate"
		% int(stats["free_slots"])).is_greater(0)
	assert_int(int(w.debug_stream_stats()["overflow_ever"])).override_failure_message(
		"streaming tripped the fail-soft drop arm; bricks were lost").is_equal(0)

	# The user-visible half: an edit under a full atlas must still allocate every brick it
	# activates. Compare the CPU probe (the contract brick_mark.comp.glsl mirrors) against
	# the GPU region tables over the op's whole brick range.
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var hit: Dictionary = w.debug_raycast(Vector3(40, 80, 40), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]
	var r: Dictionary = tool.apply_sphere_subtract(hp, 2.5)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.debug_stream_frame(cam)

	var ops := make_op(0, 0, hp, 2.5)
	var corner := (hp - Vector3.ONE * 2.6) / 0.8 # ve::op_brick_range, kBrickSize = 0.8 m
	var lo := Vector3i(floori(corner.x), floori(corner.y), floori(corner.z))
	var missing := 0
	var active := 0
	for bx in range(lo.x, lo.x + 9):
		for by in range(lo.y, lo.y + 9):
			for bz in range(lo.z, lo.z + 9):
				var brick := Vector3i(bx, by, bz)
				if not w.debug_brick_has_surface(brick, ops, 1):
					continue
				active += 1
				var region := Vector3i(floori(bx / 32.0), floori(by / 32.0), floori(bz / 32.0))
				var rslot: int = w.debug_region_map_entry(region)
				if rslot < 0 or w.debug_region_table_slot(rslot, brick) < 0:
					missing += 1
	assert_int(active).override_failure_message(
		"the edit activated no bricks; check the aim").is_greater(0)
	assert_int(missing).override_failure_message(
		"%d of %d bricks the edit activated hold no atlas slot — the shattered-terrain bug"
		% [missing, active]).is_equal(0)
	assert_int(int(w.debug_stream_stats()["overflow_ever"])).override_failure_message(
		"the edit tripped the fail-soft drop arm").is_equal(0)

func test_a_starved_atlas_heals_instead_of_keeping_the_holes() -> void:
	# The other half of the demo failure: a camera that FLIES into an over-subscribed atlas
	# passes through frames where the mark pass finds the free list empty and drops bricks.
	# A dropped brick is in no load or edit range afterwards, so without the repair sweep and
	# the give-back that follows a reported drop, those holes are permanent — the player is
	# left looking at sky through the ground. Fly in, then let it settle, and require the
	# world to be whole again.
	const SMALL_ATLAS := Vector3i(20, 10, 20)
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = SMALL_ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = ORIGIN
	w.world_size_regions = REGIONS
	w.residency_radius_m = RADIUS
	add_child(w)
	w.ensure_initialized()
	var cam := Vector3(10, 56.2, 10)
	for i in range(120): # the flight: fast enough to outrun the streamer
		cam += Vector3(0.42, 0.0, 0.42)
		w.debug_stream_frame(cam)
	assert_int(int(w.debug_stream_stats()["overflow_ever"]) & 1).override_failure_message(
		"the flight did not starve the atlas; this test is not exercising the repair path"
		).is_not_equal(0)
	for i in range(240):
		w.debug_stream_frame(cam)

	# Every brick the CPU calls active in the region under the camera must hold a slot: the
	# sweep re-marked it and the give-back made room for what it re-marked.
	var region := Vector3i(floori(cam.x / 25.6), floori(cam.y / 25.6), floori(cam.z / 25.6))
	var rslot: int = w.debug_region_map_entry(region)
	assert_int(rslot).override_failure_message(
		"the camera's own region is not resident").is_greater_equal(0)
	var active := 0
	var missing := 0
	for bx in range(region.x * 32, region.x * 32 + 32, 3):
		for bz in range(region.z * 32, region.z * 32 + 32, 3):
			for by in range(region.y * 32, region.y * 32 + 32):
				var brick := Vector3i(bx, by, bz)
				if not w.debug_brick_has_surface(brick, PackedByteArray(), 0):
					continue
				active += 1
				if w.debug_region_table_slot(rslot, brick) < 0:
					missing += 1
	assert_int(active).is_greater(0)
	assert_int(missing).override_failure_message(
		"%d of %d active bricks under the camera are still missing after the repair sweep"
		% [missing, active]).is_equal(0)

func test_job_overflow_recovers_via_force_regen() -> void:
	# Final-review Finding 3b: the job-list overflow arm. brick_mark.comp.glsl sets
	# overflow bit 1 (value 2) when the frame's job counter exceeds max_brick_jobs; the
	# streamer reads the bit next frame and force-regens the regions it marked last
	# frame, so the dropped bricks are re-enqueued (one frame of stale atlas bytes is
	# the documented cost). With an 8-job list, the first stream frame's ~6k surface
	# bricks overflow it, and the recovery frame regenerates the jobs it enqueues.
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.max_brick_jobs = 8
	w.world_origin_bricks = ORIGIN
	w.world_size_regions = REGIONS
	w.residency_radius_m = RADIUS
	add_child(w)
	w.ensure_initialized()

	# Frame 1: the first stream frame marks the camera neighbourhood (~6k surface
	# bricks) against an 8-job list — the job counter overflows.
	w.debug_stream_frame(Vector3(20, 56.2, 20))
	var a: Dictionary = w.debug_atlas_stats()
	assert_int(int(a["overflow"]) & 2).override_failure_message(
		"job-list overflow bit (2) was not set after the first stream frame"
		).is_not_equal(0)

	# Slots were still assigned during the overflowed mark: the slot assignment precedes
	# the job-count check in brick_mark.comp.glsl, so surface bricks hold valid slots.
	var rslot: int = w.debug_region_map_entry(Vector3i(0, 2, 0))
	assert_int(rslot).is_greater_equal(0)
	var slotted := 0
	for by in range(64, 76):
		var brick := Vector3i(16, by, 16)
		if w.debug_region_table_slot(rslot, brick) >= 0:
			slotted += 1
	assert_int(slotted).override_failure_message(
		"no surface bricks hold slots after the overflowed mark").is_greater(0)

	# Frame 2: the streamer saw the overflow bit and force-regens the regions it marked
	# last frame. The jobs it enqueued ARE the bricks regenerated this frame — diff one
	# against the CPU reference.
	w.debug_stream_frame(Vector3(20, 56.2, 20))
	var jobs := w.debug_jobs()
	assert_int(jobs.size()).override_failure_message(
		"recovery frame enqueued no jobs").is_greater_equal(8)
	var n := jobs.size() / 8
	assert_int(n).is_greater(0)
	var d: Dictionary = w.debug_brick_diff(
		Vector3i(jobs[0], jobs[1], jobs[2]), jobs[4], PackedByteArray(), 0)
	assert_int(int(d["sdf_max_diff"])).override_failure_message(
		"recovery frame did not regenerate the job's brick").is_less_equal(1)
	assert_bool(d["palette_match"]).is_true()
	# Fail-soft: the world keeps streaming afterwards, overflow stays contained.
	assert_int(w.debug_stream_frame(Vector3(20, 56.2, 20))).is_greater_equal(0)
