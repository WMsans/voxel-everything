extends GdUnitTestSuite
var _worlds:Array=[]
func after_test()->void:
	for w in _worlds:
		if is_instance_valid(w):w.free()
	_worlds.clear()
func world()->VoxelWorld:
	var w:VoxelWorld=ClassDB.instantiate("VoxelWorld");w.use_local_device=true
	add_child(w);_worlds.append(w);return w
func test_pairs_by_identity_and_sums_occurrences()->void:
	var d: Dictionary = world().hooks().debug_ingest_gpu_timings(PackedStringArray([
		"ve:7:lod:1:e","engine","ve:7:frame:0:b","ve:7:lod:0:b","ve:7:lod:1:b",
		"ve:7:raymarch:0:e","ve:7:frame:0:e","ve:7:raymarch:0:b","ve:7:lod:0:e"]),
		PackedInt64Array([5600,77,1000,2000,5000,1900,9000,1100,3500]),42)
	assert_bool(d["valid"]).is_true()
	assert_float(d["raymarch_gpu_ms"]).is_equal_approx(.8,.0001)
	assert_float(d["lod_gpu_ms"]).is_equal_approx(2.1,.0001)
	assert_float(d["custom_frame_gpu_ms"]).is_equal_approx(8.0,.0001)
func test_bad_pairs_are_missing_not_zero()->void:
	var d: Dictionary = world().hooks().debug_ingest_gpu_timings(PackedStringArray([
		"ve:8:frame:0:b","ve:8:frame:0:e","ve:8:ssr:0:b","ve:8:lod:0:b",
		"ve:8:lod:0:e"]),PackedInt64Array([100,900,200,700,650]),43)
	assert_float(d["ssr_gpu_ms"]).is_equal(-1.0)
	assert_float(d["lod_gpu_ms"]).is_equal(-1.0)
	assert_int(d["dropped_pairs"]).is_equal(2)
func test_new_rd_frame_gets_new_sample_id()->void:
	var w: VoxelWorld = world();var n:=PackedStringArray(["ve:9:frame:0:b","ve:9:frame:0:e"])
	var a: Dictionary = w.hooks().debug_ingest_gpu_timings(n,PackedInt64Array([100,200]),50)
	var b: Dictionary = w.hooks().debug_ingest_gpu_timings(n,PackedInt64Array([300,500]),51)
	assert_int(b["sample_id"]).is_greater(int(a["sample_id"]))
func test_synthetic_ingest_declares_microseconds_without_live_scaling()->void:
	var d: Dictionary = world().hooks().debug_ingest_gpu_timings(
		PackedStringArray(["ve:10:frame:0:b","ve:10:frame:0:e"]),
		PackedInt64Array([6000000,6016000]),52)
	assert_bool(d["valid"]).is_true()
	assert_float(d["custom_frame_gpu_ms"]).is_equal_approx(16.0,.0001)
	assert_str(d["timestamp_unit"]).is_equal("synthetic_microseconds")
	assert_str(d["timestamp_normalization"]).is_equal("none_synthetic_inputs_are_microseconds")
	assert_float(d["timestamp_scale_to_microseconds"]).is_equal_approx(1.0,.0001)

func test_stream_scope_is_a_known_pass()->void:
	var d: Dictionary = world().hooks().debug_ingest_gpu_timings(PackedStringArray([
		"ve:20:frame:0:b","ve:20:stream:0:b","ve:20:stream:0:e","ve:20:frame:0:e"]),
		PackedInt64Array([1000,1200,3200,9000]),60)
	assert_bool(d["valid"]).is_true()
	assert_float(d["stream_gpu_ms"]).is_equal_approx(2.0,.0001)

func test_unattributed_is_the_frame_minus_every_labelled_pass()->void:
	# frame = 8 ms, raymarch = 3 ms, stream = 2 ms -> 3 ms carries no label.
	var d: Dictionary = world().hooks().debug_ingest_gpu_timings(PackedStringArray([
		"ve:21:frame:0:b","ve:21:raymarch:0:b","ve:21:raymarch:0:e",
		"ve:21:stream:0:b","ve:21:stream:0:e","ve:21:frame:0:e"]),
		PackedInt64Array([1000,1000,4000,4000,6000,9000]),61)
	assert_float(d["unattributed_gpu_ms"]).is_equal_approx(3.0,.0001)

func test_unattributed_never_goes_negative()->void:
	# Overlapping scopes can sum past the frame; a negative "unattributed" would read as a
	# measurement, and it is not one.
	var d: Dictionary = world().hooks().debug_ingest_gpu_timings(PackedStringArray([
		"ve:22:frame:0:b","ve:22:raymarch:0:b","ve:22:raymarch:0:e","ve:22:frame:0:e"]),
		PackedInt64Array([1000,1000,9000,2000]),62)
	assert_float(d["unattributed_gpu_ms"]).is_greater_equal(0.0)

# --- Task 8: stored-normal telemetry in the debug dictionary -------------------
# The five normal_* keys are published verbatim by debug_stored_normal_stats() and
# always satisfy the pool's capacity/live/high-water ordering.

func _packed_normals(seed: int) -> PackedByteArray:
	var samples := 17 * 17 * 17 # kBrickSdfCount
	var bytes := PackedByteArray()
	bytes.resize(samples * 2)
	for i in range(samples):
		@warning_ignore("integer_division")
		var v: int = (seed * 2654435761 + i * 97) % 65536
		bytes.encode_u16(i * 2, v)
	return bytes

func _assert_normal_invariants(stats: Dictionary) -> void:
	assert_bool(stats.has("normal_capacity_bytes")).is_true()
	assert_bool(stats.has("normal_live_bytes")).is_true()
	assert_bool(stats.has("normal_high_water_bytes")).is_true()
	assert_bool(stats.has("normal_allocation_failures")).is_true()
	assert_bool(stats.has("normal_fallback_hits")).is_true()
	var cap := int(stats["normal_capacity_bytes"])
	var live := int(stats["normal_live_bytes"])
	var hw := int(stats["normal_high_water_bytes"])
	assert_int(cap).is_greater(0)
	assert_int(live).is_greater_equal(0)
	assert_int(hw).is_greater_equal(0)
	assert_int(live).override_failure_message(
		"live %d exceeds capacity %d" % [live, cap]).is_less_equal(cap)
	assert_int(hw).override_failure_message(
		"high-water %d exceeds capacity %d" % [hw, cap]).is_less_equal(cap)
	assert_int(live).override_failure_message(
		"live %d above high-water %d" % [live, hw]).is_less_equal(hw)
	assert_int(stats["normal_allocation_failures"]).is_greater_equal(0)
	assert_int(stats["normal_fallback_hits"]).is_greater_equal(0)

func test_debug_dictionary_publishes_the_five_normal_telemetry_keys() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	_assert_normal_invariants(w.hooks().debug_stored_normal_stats())
	# Default settings: the fixed 32 MiB budget (packed payload + both offset tables).
	assert_int(w.hooks().debug_stored_normal_stats()["normal_capacity_bytes"]).is_equal(33554432)

func test_override_upload_release_and_exhaustion_move_the_new_telemetry_keys() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	# Shrunk pool: three 17^3 payloads fit, the fourth is refused exactly once.
	w.hooks().debug_set_normal_pool_budget(65536)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()

	var before: Dictionary = w.hooks().debug_stored_normal_stats()
	_assert_normal_invariants(before)
	assert_int(before["normal_live_bytes"]).is_equal(0)
	assert_int(before["normal_high_water_bytes"]).is_equal(0)

	var o0: int = w.hooks().debug_normal_upload_override(0, _packed_normals(1))
	var o1: int = w.hooks().debug_normal_upload_override(1, _packed_normals(2))
	var o2: int = w.hooks().debug_normal_upload_override(2, _packed_normals(3))
	assert_int(o0).is_greater_equal(0)
	assert_int(o1).is_greater_equal(0)
	assert_int(o2).is_greater_equal(0)
	var mid: Dictionary = w.hooks().debug_stored_normal_stats()
	_assert_normal_invariants(mid)
	assert_int(mid["normal_live_bytes"]).is_greater(0)
	assert_int(mid["normal_high_water_bytes"]).is_equal(int(mid["normal_live_bytes"]))

	w.hooks().debug_normal_release_override(1)
	var released: Dictionary = w.hooks().debug_stored_normal_stats()
	_assert_normal_invariants(released)
	assert_int(released["normal_live_bytes"]).is_less(int(mid["normal_live_bytes"]))
	# High-water is a high-water mark: it never drops when bytes are released.
	assert_int(released["normal_high_water_bytes"]).is_equal(int(mid["normal_high_water_bytes"]))

	# The released span is reused first-fit, then the pool is exhausted fail-soft.
	var o3: int = w.hooks().debug_normal_upload_override(3, _packed_normals(4))
	assert_int(o3).is_greater_equal(0)
	var o4: int = w.hooks().debug_normal_upload_override(4, _packed_normals(5))
	assert_int(o4).is_equal(-1)
	var exhausted: Dictionary = w.hooks().debug_stored_normal_stats()
	_assert_normal_invariants(exhausted)
	assert_int(exhausted["normal_allocation_failures"]).is_equal(1)
	assert_int(exhausted["normal_fallback_hits"]).is_equal(1)

func test_island_spawn_keeps_the_new_normal_invariants(timeout := 60000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	for i in range(60):
		w.hooks().debug_stream_frame(Vector3(20.0, 56.0, 20.0))
	var before: Dictionary = w.hooks().debug_stored_normal_stats()
	_assert_normal_invariants(before)
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var after: Dictionary = w.hooks().debug_stored_normal_stats()
	_assert_normal_invariants(after)
	assert_int(after["normal_live_bytes"]).is_greater(int(before["normal_live_bytes"]))
	assert_int(after["normal_high_water_bytes"]).is_greater_equal(int(before["normal_high_water_bytes"]))
