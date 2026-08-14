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

func settle(w: VoxelWorld, cam: Vector3) -> void:
	var settled := false
	for i in range(60):
		if w.debug_stream_frame(cam) == 0:
			settled = true
			break
	assert_bool(settled).override_failure_message(
		"streamer did not settle within 60 frames").is_true()

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
