extends GdUnitTestSuite

const ATLAS := Vector3i(16, 8, 16)   # 2048 slots — enough for one surface region
const REGION_SLOTS := 4
const REGIONS := Vector3i(4, 2, 4)
# Region (0, 2, 0) spans bricks x,z in [0, 32) and y in [32, 64) -> world y [51.2, 76.8) m.
# The surface sits at 51.2 + hills(x, z), so this region holds the surface over the part
# of its footprint where hills() > 0 (measured: > 20 active bricks under the strided sweep).
const REGION := Vector3i(0, 2, 0)
const SLOT := 1

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = REGIONS
	add_child(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

func mark_whole_region(w: VoxelWorld, region: Vector3i, slot: int, force: bool) -> void:
	var lo := region * 32
	w.hooks().debug_mark_region(region, slot, lo, lo + Vector3i(31, 31, 31), 0, force)

func test_marking_allocates_exactly_the_bricks_the_cpu_calls_active() -> void:
	var w := make_world()
	mark_whole_region(w, REGION, SLOT, false)

	var gpu_active := 0
	var cpu_active := 0
	var disagreements := 0
	# Sampling every brick is 32768 round-trips through GDScript; a strided sweep over
	# 2048 of them still crosses the surface in every column it touches. Bricks are GLOBAL
	# lattice coordinates, so the sweep offsets the region-local (x, y, z) by REGION * 32
	# to land inside REGION (world y [51.2, 76.8) m for REGION.y = 2).
	for z in range(0, 32, 2):
		for y in range(0, 32):
			for x in range(0, 32, 8):
				var brick := Vector3i(REGION.x * 32 + x, REGION.y * 32 + y, REGION.z * 32 + z)
				var on_gpu: bool = w.hooks().debug_region_table_slot(SLOT, brick) >= 0
				var on_cpu: bool = w.hooks().debug_brick_has_surface(brick, PackedByteArray(), 0)
				gpu_active += 1 if on_gpu else 0
				cpu_active += 1 if on_cpu else 0
				if on_gpu != on_cpu:
					disagreements += 1
	assert_int(cpu_active).override_failure_message(
		"the chosen region holds no surface; pick another").is_greater(20)
	assert_int(disagreements).override_failure_message(
		"GPU marked %d bricks, CPU %d" % [gpu_active, cpu_active]).is_equal(0)

func test_allocation_draws_from_the_free_list_and_assigns_unique_slots() -> void:
	var w := make_world()
	var before: int = w.hooks().debug_atlas_stats()["free_slots"]
	mark_whole_region(w, REGION, SLOT, false)
	var stats: Dictionary = w.hooks().debug_atlas_stats()
	var allocated: int = before - stats["free_slots"]
	assert_int(allocated).is_greater(0)
	assert_int(stats["job_count"]).is_equal(allocated)
	assert_int(stats["overflow"]).is_equal(0)

	# Every job names a distinct atlas slot, and every slot is in range.
	var jobs: PackedInt32Array = w.hooks().debug_jobs()
	assert_int(jobs.size()).is_equal(stats["job_count"] * 8)
	var seen := {}
	for j in range(stats["job_count"]):
		var slot: int = jobs[j * 8 + 3]
		assert_int(slot).is_between(0, ATLAS.x * ATLAS.y * ATLAS.z - 1)
		assert_bool(seen.has(slot)).is_false()
		seen[slot] = true
		assert_int(jobs[j * 8 + 4]).is_equal(SLOT)  # region slot
		assert_int(jobs[j * 8 + 5]).is_equal(0)     # op count

func test_marking_twice_without_force_enqueues_nothing_new() -> void:
	var w := make_world()
	mark_whole_region(w, REGION, SLOT, false)
	var first: int = w.hooks().debug_atlas_stats()["job_count"]
	assert_int(first).is_greater(0)
	var free_after_first: int = w.hooks().debug_atlas_stats()["free_slots"]

	w.hooks().debug_reset_frame_counters()
	mark_whole_region(w, REGION, SLOT, false)
	assert_int(w.hooks().debug_atlas_stats()["job_count"]).is_equal(0)
	assert_int(w.hooks().debug_atlas_stats()["free_slots"]).is_equal(free_after_first)

func test_force_regen_re_enqueues_the_resident_bricks() -> void:
	var w := make_world()
	mark_whole_region(w, REGION, SLOT, false)
	var first: int = w.hooks().debug_atlas_stats()["job_count"]
	var free_after_first: int = w.hooks().debug_atlas_stats()["free_slots"]

	w.hooks().debug_reset_frame_counters()
	mark_whole_region(w, REGION, SLOT, true)
	assert_int(w.hooks().debug_atlas_stats()["job_count"]).is_equal(first)
	# Re-enqueueing must not allocate a second slot for a brick that already has one.
	assert_int(w.hooks().debug_atlas_stats()["free_slots"]).is_equal(free_after_first)

func test_releasing_a_region_returns_every_slot() -> void:
	var w := make_world()
	var before: int = w.hooks().debug_atlas_stats()["free_slots"]
	mark_whole_region(w, REGION, SLOT, false)
	assert_int(w.hooks().debug_atlas_stats()["free_slots"]).is_less(before)
	w.hooks().debug_release_region(SLOT)
	assert_int(w.hooks().debug_atlas_stats()["free_slots"]).is_equal(before)
	assert_int(w.hooks().debug_region_table_slot(SLOT, Vector3i(0, 0, 0))).is_equal(-1)

	# The freed slots really are usable again: a second load succeeds with no overflow.
	w.hooks().debug_reset_frame_counters()
	mark_whole_region(w, REGION, SLOT, false)
	assert_int(w.hooks().debug_atlas_stats()["overflow"]).is_equal(0)
	assert_int(w.hooks().debug_atlas_stats()["free_slots"]).is_less(before)

func test_an_edit_op_activates_bricks_the_base_field_leaves_solid() -> void:
	var w := make_world()
	# Region (0, -1, 0) spans world y in [-25.6, 0) m — solid rock under the hills.
	var region := Vector3i(0, -1, 0)
	var lo := region * 32
	mark_whole_region(w, region, 2, false)
	var solid_jobs: int = w.hooks().debug_atlas_stats()["job_count"]

	# Carve a 4 m sphere in the middle of it.
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(0); b.put_u32(0)
	b.put_float(12.8); b.put_float(-12.8); b.put_float(12.8)
	b.put_float(4.0)
	b.put_u32(0); b.put_u32(0)
	w.hooks().debug_upload_region_ops(2, b.data_array, 1)

	w.hooks().debug_reset_frame_counters()
	w.hooks().debug_mark_region(region, 2, lo, lo + Vector3i(31, 31, 31), 1, false)
	assert_int(w.hooks().debug_atlas_stats()["job_count"]).override_failure_message(
		"the carve activated no new bricks (base activated %d)" % solid_jobs).is_greater(0)

func test_exhausting_the_atlas_sets_the_overflow_bit_and_does_not_crash() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = Vector3i(2, 2, 2)   # 8 slots — far too few
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = REGIONS
	add_child(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	mark_whole_region(w, REGION, SLOT, false)
	var s: Dictionary = w.hooks().debug_atlas_stats()
	assert_int(s["overflow"] & 1).is_equal(1)
	assert_int(s["free_slots"]).is_equal(0)   # never negative: the over-decrement is undone
