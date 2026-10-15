extends GdUnitTestSuite

# Paced stream-in (WorldStreamer::run_frame's job budget). A region is probed whole at load,
# which tags and counts its surface bricks per slab; once the counts are read back its slabs
# are allocated over the following frames, as many per frame as the brick budget pays for,
# and it enters the region map -- becomes visible -- only once its last slab is allocated.
# Before this every region's bricks were generated in the frame it loaded: four regions of
# Fjords in one frame was a 130 ms hitch.
#
# Same shrunk world as test_streaming.gd (see the atlas sizing note there).
const ATLAS := Vector3i(32, 16, 32)
const REGION_SLOTS := 16
const RADIUS := 20.0
const CAM := Vector3(20, 56.2, 20)
# Well under one surface slab of the analytic hills, so every surface slab takes a frame.
const BUDGET := 64
const SETTLE_QUIET_FRAMES := 6

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.residency_radius_m = RADIUS
	add_child(w)
	w.ensure_initialized()
	return w

func settle_paced(w: VoxelWorld, cam: Vector3, budget: int, max_frames := 600) -> int:
	var quiet := 0
	for i in range(max_frames):
		quiet = quiet + 1 if w.hooks().debug_stream_frame_paced(cam, budget) == 0 else 0
		if quiet >= SETTLE_QUIET_FRAMES:
			return i
	fail("paced streamer did not settle within %d frames" % max_frames)
	return max_frames

# The column test_streaming.gd sweeps: world (12.8, *, 12.8) crosses the surface near brick
# 64. Returns [cpu_active, gpu_held] for the bricks whose owning region is PUBLISHED.
func published_column(w: VoxelWorld) -> Array:
	var cpu_active := 0
	var gpu_held := 0
	for by in range(56, 72):
		var brick := Vector3i(16, by, 16)
		var owner_slot: int = w.hooks().debug_region_map_entry(Vector3i(0, floori(by / 32.0), 0))
		if owner_slot < 0:
			continue
		if w.hooks().debug_brick_has_surface(brick, PackedByteArray(), 0):
			cpu_active += 1
			if w.hooks().debug_region_table_slot(owner_slot, brick) >= 0:
				gpu_held += 1
	return [cpu_active, gpu_held]

func test_a_paced_region_is_published_only_once_complete() -> void:
	var w := make_world()
	var staged_seen := false
	for i in range(200):
		w.hooks().debug_stream_frame_paced(CAM, BUDGET)
		var region := Vector3i(0, 2, 0)
		var resident: bool = w.hooks().debug_slot_of_region(region) >= 0
		var published: bool = w.hooks().debug_region_map_entry(region) >= 0
		if resident and not published:
			staged_seen = true
		# Whatever is published is whole: the renderer never sees half a region.
		var col := published_column(w)
		assert_int(col[1]).override_failure_message(
			"frame %d: a published region holds %d of its %d active column bricks" % [i, col[1], col[0]]
			).is_equal(col[0])
		if published:
			break
	assert_bool(staged_seen).override_failure_message(
		"the camera's region was never resident-but-unpublished: nothing was paced").is_true()

func test_paced_streaming_settles_to_the_same_world() -> void:
	var w := make_world()
	settle_paced(w, CAM, BUDGET)
	assert_bool(w.hooks().debug_region_map_consistent()).is_true()
	assert_int(w.hooks().debug_region_map_entry(Vector3i(0, 2, 0))).is_greater_equal(0)
	assert_int(w.hooks().debug_region_map_entry(Vector3i(0, 1, 0))).is_greater_equal(0)
	var col := published_column(w)
	assert_int(col[0]).override_failure_message("column holds no surface").is_greater(0)
	assert_int(col[1]).is_equal(col[0])
	assert_int(int(w.hooks().debug_stream_stats()["overflow_ever"])).is_equal(0)

# The point of the budget: no paced frame generates anything like what an unpaced frame does.
func test_a_paced_frame_generates_a_fraction_of_an_unpaced_one() -> void:
	var w := make_world()
	var unpaced_max := 0
	var quiet := 0
	for i in range(120):
		var actions: int = w.hooks().debug_stream_frame(CAM)
		unpaced_max = maxi(unpaced_max, w.hooks().debug_jobs().size() / 8)
		quiet = quiet + 1 if actions == 0 else 0
		if quiet >= SETTLE_QUIET_FRAMES:
			break
	w.free()

	w = make_world()
	var paced_max := 0
	quiet = 0
	for i in range(600):
		var actions: int = w.hooks().debug_stream_frame_paced(CAM, BUDGET)
		paced_max = maxi(paced_max, w.hooks().debug_jobs().size() / 8)
		quiet = quiet + 1 if actions == 0 else 0
		if quiet >= SETTLE_QUIET_FRAMES:
			break
	assert_int(unpaced_max).is_greater(0)
	assert_int(paced_max).override_failure_message(
		"paced frames peaked at %d jobs against %d unpaced" % [paced_max, unpaced_max]
		).is_less_equal(unpaced_max / 3)

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type); b.put_u32(material)
	b.put_float(pos.x); b.put_float(pos.y); b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0); b.put_u32(0)
	return b.data_array

# An edit landing on a region still being paced in must survive: the remaining slabs are
# marked against the op list as it is when they run, not as it was at load.
func test_an_edit_on_a_staged_region_is_in_its_bricks() -> void:
	var w := make_world()
	var hit: Dictionary = w.hooks().debug_raycast(Vector3(14.0, 80.0, 14.0), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var centre: Vector3 = hit["pos"]
	var region := Vector3i((centre / 25.6).floor())
	for i in range(50):
		w.hooks().debug_stream_frame_paced(CAM, BUDGET)
		if w.hooks().debug_slot_of_region(region) >= 0:
			break
	assert_int(w.hooks().debug_slot_of_region(region)).is_greater_equal(0)
	assert_int(w.hooks().debug_region_map_entry(region)).override_failure_message(
		"the region was published before the edit could land on it").is_less(0)
	# A crater across the column, on slabs the mark has not reached yet.
	w.hooks().debug_apply_sphere_subtract(centre, 3.0)
	settle_paced(w, CAM, BUDGET)
	var ops := make_op(0, 0, centre, 3.0)
	var rslot: int = w.hooks().debug_region_map_entry(region)
	assert_int(rslot).is_greater_equal(0)
	# The GPU holds exactly the bricks the CPU says the edited field has, across every slab.
	var cb := Vector3i((centre / 0.8).floor())
	var r0 := region * 32
	var active := 0
	var carved := 0
	for bx in range(r0.x, r0.x + 32):
		for by in range(maxi(r0.y, cb.y - 6), mini(r0.y + 32, cb.y + 6)):
			var brick := Vector3i(bx, by, cb.z)
			if not w.hooks().debug_brick_has_surface(brick, ops, 1):
				continue
			active += 1
			if not w.hooks().debug_brick_has_surface(brick, PackedByteArray(), 0):
				carved += 1
			assert_int(w.hooks().debug_region_table_slot(rslot, brick)).override_failure_message(
				"edited active brick %s holds no slot" % brick).is_greater_equal(0)
	assert_int(active).is_greater(0)
	assert_int(carved).override_failure_message("the sweep never crossed the crater").is_greater(0)
