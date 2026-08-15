extends GdUnitTestSuite

# Every GPU test in M2 must shrink the atlas: the shipping configuration allocates ~740 MB,
# and a local RenderingDevice allocates its own copy on top of the running editor's.
const ATLAS := Vector3i(8, 8, 8)      # 512 slots
const REGION_SLOTS := 8
const REGIONS := Vector3i(4, 2, 4)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = REGIONS
	add_child(w)
	return w

func test_atlas_allocates_and_reports_its_geometry() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var s: Dictionary = w.debug_atlas_stats()
	assert_int(s["slot_count"]).is_equal(512)
	assert_int(s["free_slots"]).is_equal(512)     # nothing allocated yet
	assert_int(s["region_map_entries"]).is_equal(4 * 2 * 4)
	assert_int(s["job_count"]).is_equal(0)
	assert_int(s["overflow"]).is_equal(0)

func test_texture_extents_follow_the_per_brick_strides() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	assert_object(rd).is_not_null()
	# SDF: 17 samples per brick per axis, R8 -> one byte per texel.
	assert_int(rd.texture_get_data(w.debug_sdf_atlas(), 0).size()).is_equal(136 * 136 * 136)
	# Material: 16 cells per brick per axis, R8.
	assert_int(rd.texture_get_data(w.debug_mat_atlas(), 0).size()).is_equal(128 * 128 * 128)
	# Min-max levels: 2 / 4 / 8 cells per brick per axis, RG8 -> two bytes per texel.
	assert_int(rd.texture_get_data(w.debug_mip_atlas(0), 0).size()).is_equal(16 * 16 * 16 * 2)
	assert_int(rd.texture_get_data(w.debug_mip_atlas(1), 0).size()).is_equal(32 * 32 * 32 * 2)
	assert_int(rd.texture_get_data(w.debug_mip_atlas(2), 0).size()).is_equal(64 * 64 * 64 * 2)

func test_region_map_starts_empty_and_takes_single_entry_writes() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	var before := rd.buffer_get_data(w.debug_region_map()).to_int32_array()
	assert_int(before.size()).is_equal(4 * 2 * 4)
	for v in before:
		assert_int(v).is_equal(-1)
	w.debug_set_region_map_entry(5, 3)
	var after := rd.buffer_get_data(w.debug_region_map()).to_int32_array()
	assert_int(after[5]).is_equal(3)
	assert_int(after[4]).is_equal(-1)

func test_region_tables_start_absent() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	# One region's worth is enough to prove the fill; the whole buffer is 1 MB.
	var slice := rd.buffer_get_data(w.debug_region_tables(), 0, 32768 * 4).to_int32_array()
	assert_int(slice.size()).is_equal(32768)
	for i in [0, 1, 17, 4095, 32767]:
		assert_int(slice[i]).is_equal(-1)

func test_free_list_is_a_full_permutation_of_the_slots() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	var fl := rd.buffer_get_data(w.debug_free_list()).to_int32_array()
	assert_int(fl.size()).is_equal(512)
	var seen := {}
	for v in fl:
		assert_int(v).is_between(0, 511)
		seen[v] = true
	assert_int(seen.size()).is_equal(512)

func test_frame_counters_reset_clears_the_job_count_but_not_the_overflow_bits() -> void:
	# The job count is per frame; the overflow word is STICKY. It is read back on the render
	# thread, where nothing stalls for a submit, so a per-frame reset would race the readback
	# and a frame's worth of dropped bricks could go unreported — and an unreported drop is a
	# hole in the world that nothing ever comes back to fill. Whoever acts on the bits is the
	# one that clears them.
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	rd.buffer_update(w.debug_frame_counters(), 0, 8,
		PackedInt32Array([7, 3]).to_byte_array())
	assert_int(w.debug_atlas_stats()["job_count"]).is_equal(7)
	w.debug_reset_frame_counters()
	var s: Dictionary = w.debug_atlas_stats()
	assert_int(s["job_count"]).is_equal(0)
	assert_int(s["overflow"]).override_failure_message(
		"the overflow bits must survive a frame reset").is_equal(3)

func test_region_ops_upload_byte_for_byte() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	# ve::EditOp: type, material, pos[3], radius, pad[2].
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(0); b.put_u32(9)
	b.put_float(1.5); b.put_float(2.5); b.put_float(3.5)
	b.put_float(4.5)
	b.put_u32(0); b.put_u32(0)
	w.debug_upload_region_ops(2, b.data_array, 1)

	# Region slot 2 starts at 2 * 256 ops * 32 bytes.
	var got := rd.buffer_get_data(w.debug_op_pool(), 2 * 256 * 32, 32)
	assert_array(Array(got)).is_equal(Array(b.data_array))
	var counts := rd.buffer_get_data(w.debug_op_counts()).to_int32_array()
	assert_int(counts[2]).is_equal(1)
	assert_int(counts[0]).is_equal(0)

func test_teardown_is_idempotent_and_survives_re_init() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	w.debug_teardown_atlas()
	w.debug_teardown_atlas()
	assert_bool(w.debug_init_atlas()).is_true()
	assert_int(w.debug_atlas_stats()["free_slots"]).is_equal(512)
